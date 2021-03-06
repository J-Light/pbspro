/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file    req_message.c
 *
 * @brief
 * 		req_message.c - functions dealing with sending a message to a running job.
 *
 * Functions included are:
 * 	req_messagejob()
 * 	post_message_req()
 * 	post_py_spawn_req()
 * 	req_py_spawn()
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>
#include "libpbs.h"
#include <signal.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "job.h"
#include "work_task.h"
#include "pbs_error.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "acct.h"

/* Private Function local to this file */

static void post_message_req(struct work_task *);

/* Global Data Items: */

extern char *msg_messagejob;

extern job  *chk_job_request(char *, struct batch_request *, int *);



/**
 * @brief
 * 		req_messagejob - service the Message Job Request
 *
 *		This request sends (via MOM) a message to a running job.
 *
 * @param[in]	preq	- Pointer to batch request
 */

void
req_messagejob(struct batch_request *preq)
{
	int               jt;            /* job type */
	job		 *pjob;
	int		  rc;

	if ((pjob = chk_job_request(preq->rq_ind.rq_message.rq_jid, preq, &jt)) == 0)
		return;

	if (jt != IS_ARRAY_NO) {
		reply_text(preq, PBSE_NOSUP, "not supported for Array Jobs");
		return;
	}

	/* the job must be running */

	if (pjob->ji_qs.ji_state != JOB_STATE_RUNNING) {
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}

	/* pass the request on to MOM */

	rc = relay_to_mom(pjob, preq, post_message_req);
	if (rc)
		req_reject(rc, 0, preq);	/* unable to get to MOM */

	/* After MOM acts and replies to us, we pick up in post_message_req() */
}

/**
 * @brief
 * 		post_message_req - complete a Message Job Request
 *
 * @param[in]	pwt	-	work task structure
 */

static void
post_message_req(struct work_task *pwt)
{
	struct batch_request *preq;

	if (pwt->wt_aux2 != 1) /* not rpp */
		svr_disconnect(pwt->wt_event);	/* close connection to MOM */
	preq = pwt->wt_parm1;
	preq->rq_conn = preq->rq_orgconn;  /* restore socket to client */

	(void)sprintf(log_buffer, msg_messagejob, preq->rq_reply.brp_code);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
		preq->rq_ind.rq_message.rq_jid, log_buffer);
	if (preq->rq_reply.brp_code)
		req_reject(preq->rq_reply.brp_code, 0, preq);
	else
		reply_ack(preq);
}

/**
 * @brief
 * 		post_py_spawn_req - complete a py_spawn Job Request
 *
 * @param[in]	pwt	-	work task structure
 */

static void
post_py_spawn_req(struct work_task *pwt)
{
	struct batch_request *preq;
	char	tmp_buf[128] = "";

	if (pwt->wt_aux2 != 1) /* not rpp */
		svr_disconnect(pwt->wt_event);	/* close connection to MOM */
	preq = pwt->wt_parm1;
	preq->rq_conn = preq->rq_orgconn;  /* restore socket to client */

	if (preq->rq_reply.brp_code == 0)
		sprintf(tmp_buf, " exit value %d", preq->rq_reply.brp_auxcode);
	sprintf(log_buffer, "Python spawn status %d%s",
		preq->rq_reply.brp_code, tmp_buf);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
		preq->rq_ind.rq_py_spawn.rq_jid, log_buffer);
	reply_send(preq);
}

/**
 * @brief
 * 		req_py_spawn - service the Python Spawn Request
 *
 * @param[in]	preq	- Pointer to batch request
 */

void
req_py_spawn(struct batch_request *preq)
{
	int             jt;		/* job type */
	job		*pjob;
	int		rc;
	char		*jid = preq->rq_ind.rq_py_spawn.rq_jid;
	int		i, offset;

	/*
	 ** Returns job pointer for singleton job or "parent" of
	 ** an array job.
	 */
	pjob = chk_job_request(jid, preq, &jt);
	if (pjob == NULL)
		return;

	/* see if requestor is the job owner */
	if (svr_chk_owner(preq, pjob) != 0) {
		req_reject(PBSE_PERM, 0, preq);
		return;
	}

	if (jt == IS_ARRAY_NO) {		/* a regular job is okay */
		/* the job must be running */
		if ((pjob->ji_qs.ji_state != JOB_STATE_RUNNING) ||
			(pjob->ji_qs.ji_substate !=
			JOB_SUBSTATE_RUNNING)) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
	}
	else if (jt == IS_ARRAY_Single) {	/* a single subjob is okay */

		offset = subjob_index_to_offset(pjob,
			get_index_from_jid(jid));
		if (offset == -1) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}

		i = get_subjob_state(pjob, offset);
		if (i == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		}

		if (i != JOB_STATE_RUNNING) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
		pjob = find_job(jid);	/* get ptr to the subjob */
		if (pjob == NULL) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}
		if (pjob->ji_qs.ji_substate != JOB_SUBSTATE_RUNNING) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
	} else {
		reply_text(preq, PBSE_NOSUP,
			"not supported for Array Jobs or multiple sub-jobs");
		return;
	}

	/*
	 ** Pass the request on to MOM.  If this works, the function
	 ** post_py_spawn_req will be called to handle the reply.
	 ** If it fails, send the reply now.
	 */
	rc = relay_to_mom(pjob, preq, post_py_spawn_req);
	if (rc)
		req_reject(rc, 0, preq);	/* unable to get to MOM */
}

/**
 * @brief
 * 	Service the PBS_BATCH_RelnodesJob Request.
 *
 * @param[in]	preq - the request structure.
 *
 * @rerturn void
 *
 */

void
req_relnodesjob(struct batch_request *preq)
{
	int             jt;		/* job type */
	job		*pjob;
	int		rc;
	char		*jid;
	int		i, offset;
	char		*nodeslist = NULL;
	char		msg[LOG_BUF_SIZE];

 
	if (preq == NULL)
		return;

	jid = preq->rq_ind.rq_relnodes.rq_jid;
	if (jid == NULL)
		return;

	/*
	 ** Returns job pointer for singleton job or "parent" of
	 ** an array job.
	 */
	pjob = chk_job_request(jid, preq, &jt);
	if (pjob == NULL) {
		return;
	}

	if (jt == IS_ARRAY_NO) {		/* a regular job is okay */
		/* the job must be running */
		if ((pjob->ji_qs.ji_state != JOB_STATE_RUNNING) ||
			(pjob->ji_qs.ji_substate !=
			JOB_SUBSTATE_RUNNING)) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
	}
	else if (jt == IS_ARRAY_Single) {	/* a single subjob is okay */

		offset = subjob_index_to_offset(pjob,
			get_index_from_jid(jid));
		if (offset == -1) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}

		i = get_subjob_state(pjob, offset);
		if (i == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		}

		if (i != JOB_STATE_RUNNING) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
		pjob = find_job(jid);	/* get ptr to the subjob */
		if (pjob == NULL) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}
		if (pjob->ji_qs.ji_substate != JOB_SUBSTATE_RUNNING) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
	} else {
		reply_text(preq, PBSE_NOSUP,
			"not supported for Array Jobs or multiple sub-jobs");
		return;
	}

	nodeslist = preq->rq_ind.rq_relnodes.rq_node_list;

	if ((nodeslist != NULL) && (nodeslist[0] == '\0')) {
		nodeslist = NULL;
	}
	rc = free_sister_vnodes(pjob, nodeslist, msg, LOG_BUF_SIZE, preq);

	if (rc != 0) {
		reply_text(preq, PBSE_SYSTEM, msg);
	}
}
