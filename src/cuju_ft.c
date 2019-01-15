#define _GNU_SOURCE

#include <common/splice.h>
#include <errno.h>
#include <types/cuju_ft.h>
#include <types/global.h>
#include <types/fd.h>

#include <proto/fd.h>
#include <proto/connection.h>
#include <proto/proto_tcp.h>
#include <proto/stream_interface.h>

#ifdef DEBUG_FULL
#include <assert.h>
#endif

#if ENABLE_CUJU_FT
int fd_list_migration = 0;

unsigned long ft_get_flushcnt() 
{
    static unsigned long flush_count = 0;

    return flush_count++;
}

int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean)
{
    int ret = 0;
    static unsigned long retry_cnt = 0;

    if (!source->data) {
        return 0;
    }
    if (dest->data) {
#ifdef DEBUG_FULL        
        assert(1);
#else
        return 0;
#endif        
    }

    while (1) {
        if (clean) {
		    ret = splice(source->cons, NULL, dest->prod, NULL, 
					     source->data, SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
        }
        else {
            ret = tee(source->cons, dest->prod, source->data, SPLICE_F_NONBLOCK);
        }

        if (ret < 0) {
            if (errno == EAGAIN) {
                retry_cnt++;
                ret = 0;
                continue;
            }
            
            return ret;
        }

        break;
    }

    dest->data = source->data;
    dest->in_fd = source->in_fd;
    dest->out_fd = source->out_fd;

    return ret;
}



/* Cuju IPC handler callback */
void cuju_fd_handler(int fd)
{
	struct connection *conn = fdtab[fd].owner;
    unsigned int flags;
    int io_available = 0;

	if (unlikely(!conn))
		return;

	conn_refresh_polling_flags(conn);
	conn->flags |= CO_FL_WILL_UPDATE;

    /* ensure to call the wake handler upon error */
	flags = conn->flags & ~CO_FL_ERROR;         

    printf("cuju_fd_handler fd is %d\n", fd);

    //fd_stop_recv(fd);
process_handshake:
	/* The handshake callbacks are called in sequence. If either of them is
	 * missing something, it must enable the required polling at the socket
	 * layer of the connection. Polling state is not guaranteed when entering
	 * these handlers, so any handshake handler which does not complete its
	 * work must explicitly disable events it's not interested in. Error
	 * handling is also performed here in order to reduce the number of tests
	 * around.
	 */
	while (unlikely(conn->flags & (CO_FL_HANDSHAKE | CO_FL_ERROR))) {
		if (unlikely(conn->flags & CO_FL_ERROR))
			goto leave;

		if (conn->flags & CO_FL_ACCEPT_CIP)
			if (!conn_recv_netscaler_cip(conn, CO_FL_ACCEPT_CIP))
				goto leave;

		if (conn->flags & CO_FL_ACCEPT_PROXY)
			if (!conn_recv_proxy(conn, CO_FL_ACCEPT_PROXY))
				goto leave;

		if (conn->flags & CO_FL_SEND_PROXY)
			if (!conn_si_send_proxy(conn, CO_FL_SEND_PROXY))
				goto leave;
#ifdef USE_OPENSSL
		if (conn->flags & CO_FL_SSL_WAIT_HS)
			if (!ssl_sock_handshake(conn, CO_FL_SSL_WAIT_HS))
				goto leave;
#endif
	}

	/* Once we're purely in the data phase, we disable handshake polling */
	if (!(conn->flags & CO_FL_POLL_SOCK))
		__conn_sock_stop_both(conn);

	/* The connection owner might want to be notified about an end of
	 * handshake indicating the connection is ready, before we proceed with
	 * any data exchange. The callback may fail and cause the connection to
	 * be destroyed, thus we must not use it anymore and should immediately
	 * leave instead. The caller must immediately unregister itself once
	 * called.
	 */
	if (conn->xprt_done_cb && conn->xprt_done_cb(conn) < 0)
		return;
		
	if (conn->xprt && fd_send_ready(fd) &&
		((conn->flags & (CO_FL_XPRT_WR_ENA|CO_FL_ERROR|CO_FL_HANDSHAKE)) == CO_FL_XPRT_WR_ENA)) {
		/* force reporting of activity by clearing the previous flags :
		 * we'll have at least ERROR or CONNECTED at the end of an I/O,
		 * both of which will be detected below.
		 */
		flags = 0;
		if (conn->send_wait != NULL) {
			conn->send_wait->events &= ~SUB_RETRY_SEND;
			tasklet_wakeup(conn->send_wait->task);
			conn->send_wait = NULL;
		} else
			io_available = 1;
		__conn_xprt_stop_send(conn);
	}

	/* The data transfer starts here and stops on error and handshakes. Note
	 * that we must absolutely test conn->xprt at each step in case it suddenly
	 * changes due to a quick unexpected close().
	 */
	if (conn->xprt && fd_recv_ready(fd) &&
	    ((conn->flags & (CO_FL_XPRT_RD_ENA|CO_FL_WAIT_ROOM|CO_FL_ERROR|CO_FL_HANDSHAKE)) == CO_FL_XPRT_RD_ENA)) {
		/* force reporting of activity by clearing the previous flags :
		 * we'll have at least ERROR or CONNECTED at the end of an I/O,
		 * both of which will be detected below.
		 */
		flags = 0;
		if (conn->recv_wait) {
			conn->recv_wait->events &= ~SUB_RETRY_RECV;
			tasklet_wakeup(conn->recv_wait->task);
			conn->recv_wait = NULL;
		} else
			io_available = 1;
		__conn_xprt_stop_recv(conn);
	}

	/* It may happen during the data phase that a handshake is
	 * enabled again (eg: SSL)
	 */
	if (unlikely(conn->flags & (CO_FL_HANDSHAKE | CO_FL_ERROR)))
		goto process_handshake;

	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN)) {
		/* still waiting for a connection to establish and nothing was
		 * attempted yet to probe the connection. Then let's retry the
		 * connect().
		 */
		if (!tcp_connect_probe(conn))
			goto leave;
	}
 leave:
	/* Verify if the connection just established. */
	if (unlikely(!(conn->flags & (CO_FL_WAIT_L4_CONN | CO_FL_WAIT_L6_CONN | CO_FL_CONNECTED))))
		conn->flags |= CO_FL_CONNECTED;

	/* The connection owner might want to be notified about failures to
	 * complete the handshake. The callback may fail and cause the
	 * connection to be destroyed, thus we must not use it anymore and
	 * should immediately leave instead. The caller must immediately
	 * unregister itself once called.
	 */
	if (((conn->flags ^ flags) & CO_FL_NOTIFY_DONE) &&
	    conn->xprt_done_cb && conn->xprt_done_cb(conn) < 0)
		return;

	/* The wake callback is normally used to notify the data layer about
	 * data layer activity (successful send/recv), connection establishment,
	 * shutdown and fatal errors. We need to consider the following
	 * situations to wake up the data layer :
	 *  - change among the CO_FL_NOTIFY_DATA flags :
	 *      {DATA,SOCK}_{RD,WR}_SH, ERROR,
	 *  - absence of any of {L4,L6}_CONN and CONNECTED, indicating the
	 *    end of handshake and transition to CONNECTED
	 *  - raise of CONNECTED with HANDSHAKE down
	 *  - end of HANDSHAKE with CONNECTED set
	 *  - regular data layer activity
	 *
	 * Note that the wake callback is allowed to release the connection and
	 * the fd (and return < 0 in this case).
	 */
	if ((io_available || (((conn->flags ^ flags) & CO_FL_NOTIFY_DATA) ||
	     ((flags & (CO_FL_CONNECTED|CO_FL_HANDSHAKE)) != CO_FL_CONNECTED &&
	      (conn->flags & (CO_FL_CONNECTED|CO_FL_HANDSHAKE)) == CO_FL_CONNECTED))) &&
	    conn->mux->wake && conn->mux->wake(conn) < 0)
		return;

	/* commit polling changes */
	conn->flags &= ~CO_FL_WILL_UPDATE;
	conn_cond_update_polling(conn);

 
	return;
}

#if 0
/* Processes the client, server, request and response jobs of a stream task,
 * then puts it back to the wait queue in a clean state, or cleans up its
 * resources if it must be deleted. Returns in <next> the date the task wants
 * to be woken up, or TICK_ETERNITY. In order not to call all functions for
 * nothing too many times, the request and response buffers flags are monitored
 * and each function is called only if at least another function has changed at
 * least one flag it is interested in.
 */
struct task *cuju_process_stream(struct task *t, void *context, unsigned short state)
{
	struct server *srv;
	struct stream *s = context;
	struct session *sess = s->sess;
	unsigned int rqf_last, rpf_last;
	unsigned int rq_prod_last, rq_cons_last;
	unsigned int rp_cons_last, rp_prod_last;
	unsigned int req_ana_back;
	struct channel *req, *res;
	struct stream_interface *si_f, *si_b;

	activity[tid].stream++;

	req = &s->req;
	res = &s->res;

	si_f = &s->si[0];
	si_b = &s->si[1];

	/* First, attempt to receive pending data from I/O layers */
	si_sync_recv(si_f);
	si_sync_recv(si_b);

redo:

	//DPRINTF(stderr, "%s:%d: cs=%d ss=%d(%d) rqf=0x%08x rpf=0x%08x\n", __FUNCTION__, __LINE__,
	//        si_f->state, si_b->state, si_b->err_type, req->flags, res->flags);

	/* this data may be no longer valid, clear it */
	if (s->txn)
		memset(&s->txn->auth, 0, sizeof(s->txn->auth));

	/* This flag must explicitly be set every time */
	req->flags &= ~(CF_READ_NOEXP|CF_WAKE_WRITE);
	res->flags &= ~(CF_READ_NOEXP|CF_WAKE_WRITE);

	/* Keep a copy of req/rep flags so that we can detect shutdowns */
	rqf_last = req->flags & ~CF_MASK_ANALYSER;
	rpf_last = res->flags & ~CF_MASK_ANALYSER;

	/* we don't want the stream interface functions to recursively wake us up */
	si_f->flags |= SI_FL_DONT_WAKE;
	si_b->flags |= SI_FL_DONT_WAKE;

	/* update pending events */
	s->pending_events |= (state & TASK_WOKEN_ANY);

	/* 1a: Check for low level timeouts if needed. We just set a flag on
	 * stream interfaces when their timeouts have expired.
	 */
	if (unlikely(s->pending_events & TASK_WOKEN_TIMER)) {
		si_check_timeouts(si_f);
		si_check_timeouts(si_b);

		/* check channel timeouts, and close the corresponding stream interfaces
		 * for future reads or writes. Note: this will also concern upper layers
		 * but we do not touch any other flag. We must be careful and correctly
		 * detect state changes when calling them.
		 */

		channel_check_timeouts(req);

		if (unlikely((req->flags & (CF_SHUTW|CF_WRITE_TIMEOUT)) == CF_WRITE_TIMEOUT)) {
			si_b->flags |= SI_FL_NOLINGER;
			si_shutw(si_b);
		}

		if (unlikely((req->flags & (CF_SHUTR|CF_READ_TIMEOUT)) == CF_READ_TIMEOUT)) {
			if (si_f->flags & SI_FL_NOHALF)
				si_f->flags |= SI_FL_NOLINGER;
			si_shutr(si_f);
		}

		channel_check_timeouts(res);

		if (unlikely((res->flags & (CF_SHUTW|CF_WRITE_TIMEOUT)) == CF_WRITE_TIMEOUT)) {
			si_f->flags |= SI_FL_NOLINGER;
			si_shutw(si_f);
		}

		if (unlikely((res->flags & (CF_SHUTR|CF_READ_TIMEOUT)) == CF_READ_TIMEOUT)) {
			if (si_b->flags & SI_FL_NOHALF)
				si_b->flags |= SI_FL_NOLINGER;
			si_shutr(si_b);
		}

		if (HAS_FILTERS(s))
			flt_stream_check_timeouts(s);

		/* Once in a while we're woken up because the task expires. But
		 * this does not necessarily mean that a timeout has been reached.
		 * So let's not run a whole stream processing if only an expiration
		 * timeout needs to be refreshed.
		 */
		if (!((req->flags | res->flags) &
		      (CF_SHUTR|CF_READ_ACTIVITY|CF_READ_TIMEOUT|CF_SHUTW|
		       CF_WRITE_ACTIVITY|CF_WRITE_TIMEOUT|CF_ANA_TIMEOUT)) &&
		    !((si_f->flags | si_b->flags) & (SI_FL_EXP|SI_FL_ERR)) &&
		    ((s->pending_events & TASK_WOKEN_ANY) == TASK_WOKEN_TIMER)) {
			si_f->flags &= ~SI_FL_DONT_WAKE;
			si_b->flags &= ~SI_FL_DONT_WAKE;
			goto update_exp_and_leave;
		}
	}

	/* below we may emit error messages so we have to ensure that we have
	 * our buffers properly allocated.
	 */
	if (!stream_alloc_work_buffer(s)) {
		/* No buffer available, we've been subscribed to the list of
		 * buffer waiters, let's wait for our turn.
		 */
		si_f->flags &= ~SI_FL_DONT_WAKE;
		si_b->flags &= ~SI_FL_DONT_WAKE;
		goto update_exp_and_leave;
	}

	/* 1b: check for low-level errors reported at the stream interface.
	 * First we check if it's a retryable error (in which case we don't
	 * want to tell the buffer). Otherwise we report the error one level
	 * upper by setting flags into the buffers. Note that the side towards
	 * the client cannot have connect (hence retryable) errors. Also, the
	 * connection setup code must be able to deal with any type of abort.
	 */
	srv = objt_server(s->target);
	if (unlikely(si_f->flags & SI_FL_ERR)) {
		if (si_f->state == SI_ST_EST || si_f->state == SI_ST_DIS) {
			si_shutr(si_f);
			si_shutw(si_f);
			si_report_error(si_f);
			if (!(req->analysers) && !(res->analysers)) {
				HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				if (!(s->flags & SF_ERR_MASK))
					s->flags |= SF_ERR_CLICL;
				if (!(s->flags & SF_FINST_MASK))
					s->flags |= SF_FINST_D;
			}
		}
	}

	if (unlikely(si_b->flags & SI_FL_ERR)) {
		if (si_b->state == SI_ST_EST || si_b->state == SI_ST_DIS) {
			si_shutr(si_b);
			si_shutw(si_b);
			si_report_error(si_b);
			HA_ATOMIC_ADD(&s->be->be_counters.failed_resp, 1);
			if (srv)
				HA_ATOMIC_ADD(&srv->counters.failed_resp, 1);
			if (!(req->analysers) && !(res->analysers)) {
				HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				if (!(s->flags & SF_ERR_MASK))
					s->flags |= SF_ERR_SRVCL;
				if (!(s->flags & SF_FINST_MASK))
					s->flags |= SF_FINST_D;
			}
		}
		/* note: maybe we should process connection errors here ? */
	}

	if (si_b->state == SI_ST_CON) {
		/* we were trying to establish a connection on the server side,
		 * maybe it succeeded, maybe it failed, maybe we timed out, ...
		 */
		if (unlikely(!sess_update_st_con_tcp(s)))
			sess_update_st_cer(s);
		else if (si_b->state == SI_ST_EST)
			sess_establish(s);

		/* state is now one of SI_ST_CON (still in progress), SI_ST_EST
		 * (established), SI_ST_DIS (abort), SI_ST_CLO (last error),
		 * SI_ST_ASS/SI_ST_TAR/SI_ST_REQ for retryable errors.
		 */
	}

	rq_prod_last = si_f->state;
	rq_cons_last = si_b->state;
	rp_cons_last = si_f->state;
	rp_prod_last = si_b->state;

 resync_stream_interface:
	/* Check for connection closure */

	DPRINTF(stderr,
		"[%u] %s:%d: task=%p s=%p, sfl=0x%08x, rq=%p, rp=%p, exp(r,w)=%u,%u rqf=%08x rpf=%08x rqh=%lu rqt=%lu rph=%lu rpt=%lu cs=%d ss=%d, cet=0x%x set=0x%x retr=%d\n",
		now_ms, __FUNCTION__, __LINE__,
		t,
		s, s->flags,
		req, res,
		req->rex, res->wex,
		req->flags, res->flags,
		ci_data(req), co_data(req), ci_data(res), co_data(res), si_f->state, si_b->state,
		si_f->err_type, si_b->err_type,
		si_b->conn_retries);

	/* nothing special to be done on client side */
	if (unlikely(si_f->state == SI_ST_DIS))
		si_f->state = SI_ST_CLO;

	/* When a server-side connection is released, we have to count it and
	 * check for pending connections on this server.
	 */
	if (unlikely(si_b->state == SI_ST_DIS)) {
		si_b->state = SI_ST_CLO;
		srv = objt_server(s->target);
		if (srv) {
			if (s->flags & SF_CURR_SESS) {
				s->flags &= ~SF_CURR_SESS;
				HA_ATOMIC_SUB(&srv->cur_sess, 1);
			}
			sess_change_server(s, NULL);
			if (may_dequeue_tasks(srv, s->be))
				process_srv_queue(srv);
		}
	}

	/*
	 * Note: of the transient states (REQ, CER, DIS), only REQ may remain
	 * at this point.
	 */

 resync_request:
	/* Analyse request */
	if (((req->flags & ~rqf_last) & CF_MASK_ANALYSER) ||
	    ((req->flags ^ rqf_last) & CF_MASK_STATIC) ||
	    (req->analysers && (req->flags & CF_SHUTW)) ||
	    si_f->state != rq_prod_last ||
	    si_b->state != rq_cons_last ||
	    s->pending_events & TASK_WOKEN_MSG) {
		unsigned int flags = req->flags;

		if (si_f->state >= SI_ST_EST) {
			int max_loops = global.tune.maxpollevents;
			unsigned int ana_list;
			unsigned int ana_back;

			/* it's up to the analysers to stop new connections,
			 * disable reading or closing. Note: if an analyser
			 * disables any of these bits, it is responsible for
			 * enabling them again when it disables itself, so
			 * that other analysers are called in similar conditions.
			 */
			channel_auto_read(req);
			channel_auto_connect(req);
			channel_auto_close(req);

			/* We will call all analysers for which a bit is set in
			 * req->analysers, following the bit order from LSB
			 * to MSB. The analysers must remove themselves from
			 * the list when not needed. Any analyser may return 0
			 * to break out of the loop, either because of missing
			 * data to take a decision, or because it decides to
			 * kill the stream. We loop at least once through each
			 * analyser, and we may loop again if other analysers
			 * are added in the middle.
			 *
			 * We build a list of analysers to run. We evaluate all
			 * of these analysers in the order of the lower bit to
			 * the higher bit. This ordering is very important.
			 * An analyser will often add/remove other analysers,
			 * including itself. Any changes to itself have no effect
			 * on the loop. If it removes any other analysers, we
			 * want those analysers not to be called anymore during
			 * this loop. If it adds an analyser that is located
			 * after itself, we want it to be scheduled for being
			 * processed during the loop. If it adds an analyser
			 * which is located before it, we want it to switch to
			 * it immediately, even if it has already been called
			 * once but removed since.
			 *
			 * In order to achieve this, we compare the analyser
			 * list after the call with a copy of it before the
			 * call. The work list is fed with analyser bits that
			 * appeared during the call. Then we compare previous
			 * work list with the new one, and check the bits that
			 * appeared. If the lowest of these bits is lower than
			 * the current bit, it means we have enabled a previous
			 * analyser and must immediately loop again.
			 */

			ana_list = ana_back = req->analysers;
			while (ana_list && max_loops--) {
				/* Warning! ensure that analysers are always placed in ascending order! */
				ANALYZE    (s, req, flt_start_analyze,          ana_list, ana_back, AN_REQ_FLT_START_FE);
				FLT_ANALYZE(s, req, tcp_inspect_request,        ana_list, ana_back, AN_REQ_INSPECT_FE);
				FLT_ANALYZE(s, req, http_wait_for_request,      ana_list, ana_back, AN_REQ_WAIT_HTTP);
				FLT_ANALYZE(s, req, http_wait_for_request_body, ana_list, ana_back, AN_REQ_HTTP_BODY);
				FLT_ANALYZE(s, req, http_process_req_common,    ana_list, ana_back, AN_REQ_HTTP_PROCESS_FE, sess->fe);
				FLT_ANALYZE(s, req, process_switching_rules,    ana_list, ana_back, AN_REQ_SWITCHING_RULES);
				ANALYZE    (s, req, flt_start_analyze,          ana_list, ana_back, AN_REQ_FLT_START_BE);
				FLT_ANALYZE(s, req, tcp_inspect_request,        ana_list, ana_back, AN_REQ_INSPECT_BE);
				FLT_ANALYZE(s, req, http_process_req_common,    ana_list, ana_back, AN_REQ_HTTP_PROCESS_BE, s->be);
				FLT_ANALYZE(s, req, http_process_tarpit,        ana_list, ana_back, AN_REQ_HTTP_TARPIT);
				FLT_ANALYZE(s, req, process_server_rules,       ana_list, ana_back, AN_REQ_SRV_RULES);
				FLT_ANALYZE(s, req, http_process_request,       ana_list, ana_back, AN_REQ_HTTP_INNER);
				FLT_ANALYZE(s, req, tcp_persist_rdp_cookie,     ana_list, ana_back, AN_REQ_PRST_RDP_COOKIE);
				FLT_ANALYZE(s, req, process_sticking_rules,     ana_list, ana_back, AN_REQ_STICKING_RULES);
				ANALYZE    (s, req, flt_analyze_http_headers,   ana_list, ana_back, AN_REQ_FLT_HTTP_HDRS);
				ANALYZE    (s, req, http_request_forward_body,  ana_list, ana_back, AN_REQ_HTTP_XFER_BODY);
				ANALYZE    (s, req, pcli_wait_for_request,      ana_list, ana_back, AN_REQ_WAIT_CLI);
				ANALYZE    (s, req, flt_xfer_data,              ana_list, ana_back, AN_REQ_FLT_XFER_DATA);
				ANALYZE    (s, req, flt_end_analyze,            ana_list, ana_back, AN_REQ_FLT_END);
				break;
			}
		}

		rq_prod_last = si_f->state;
		rq_cons_last = si_b->state;
		req->flags &= ~CF_WAKE_ONCE;
		rqf_last = req->flags;

		if ((req->flags ^ flags) & CF_MASK_STATIC)
			goto resync_request;
	}

	/* we'll monitor the request analysers while parsing the response,
	 * because some response analysers may indirectly enable new request
	 * analysers (eg: HTTP keep-alive).
	 */
	req_ana_back = req->analysers;

 resync_response:
	/* Analyse response */

	if (((res->flags & ~rpf_last) & CF_MASK_ANALYSER) ||
		 (res->flags ^ rpf_last) & CF_MASK_STATIC ||
		 (res->analysers && (res->flags & CF_SHUTW)) ||
		 si_f->state != rp_cons_last ||
		 si_b->state != rp_prod_last ||
		 s->pending_events & TASK_WOKEN_MSG) {
		unsigned int flags = res->flags;

		if (si_b->state >= SI_ST_EST) {
			int max_loops = global.tune.maxpollevents;
			unsigned int ana_list;
			unsigned int ana_back;

			/* it's up to the analysers to stop disable reading or
			 * closing. Note: if an analyser disables any of these
			 * bits, it is responsible for enabling them again when
			 * it disables itself, so that other analysers are called
			 * in similar conditions.
			 */
			channel_auto_read(res);
			channel_auto_close(res);

			/* We will call all analysers for which a bit is set in
			 * res->analysers, following the bit order from LSB
			 * to MSB. The analysers must remove themselves from
			 * the list when not needed. Any analyser may return 0
			 * to break out of the loop, either because of missing
			 * data to take a decision, or because it decides to
			 * kill the stream. We loop at least once through each
			 * analyser, and we may loop again if other analysers
			 * are added in the middle.
			 */

			ana_list = ana_back = res->analysers;
			while (ana_list && max_loops--) {
				/* Warning! ensure that analysers are always placed in ascending order! */
				ANALYZE    (s, res, flt_start_analyze,          ana_list, ana_back, AN_RES_FLT_START_FE);
				ANALYZE    (s, res, flt_start_analyze,          ana_list, ana_back, AN_RES_FLT_START_BE);
				FLT_ANALYZE(s, res, tcp_inspect_response,       ana_list, ana_back, AN_RES_INSPECT);
				FLT_ANALYZE(s, res, http_wait_for_response,     ana_list, ana_back, AN_RES_WAIT_HTTP);
				FLT_ANALYZE(s, res, process_store_rules,        ana_list, ana_back, AN_RES_STORE_RULES);
				FLT_ANALYZE(s, res, http_process_res_common,    ana_list, ana_back, AN_RES_HTTP_PROCESS_BE, s->be);
				ANALYZE    (s, res, flt_analyze_http_headers,   ana_list, ana_back, AN_RES_FLT_HTTP_HDRS);
				ANALYZE    (s, res, http_response_forward_body, ana_list, ana_back, AN_RES_HTTP_XFER_BODY);
				ANALYZE    (s, res, pcli_wait_for_response,     ana_list, ana_back, AN_RES_WAIT_CLI);
				ANALYZE    (s, res, flt_xfer_data,              ana_list, ana_back, AN_RES_FLT_XFER_DATA);
				ANALYZE    (s, res, flt_end_analyze,            ana_list, ana_back, AN_RES_FLT_END);
				break;
			}
		}

		rp_cons_last = si_f->state;
		rp_prod_last = si_b->state;
		res->flags &= ~CF_WAKE_ONCE;
		rpf_last = res->flags;

		if ((res->flags ^ flags) & CF_MASK_STATIC)
			goto resync_response;
	}

	/* maybe someone has added some request analysers, so we must check and loop */
	if (req->analysers & ~req_ana_back)
		goto resync_request;

	if ((req->flags & ~rqf_last) & CF_MASK_ANALYSER)
		goto resync_request;

	/* FIXME: here we should call protocol handlers which rely on
	 * both buffers.
	 */


	/*
	 * Now we propagate unhandled errors to the stream. Normally
	 * we're just in a data phase here since it means we have not
	 * seen any analyser who could set an error status.
	 */
	srv = objt_server(s->target);
	if (unlikely(!(s->flags & SF_ERR_MASK))) {
		if (req->flags & (CF_READ_ERROR|CF_READ_TIMEOUT|CF_WRITE_ERROR|CF_WRITE_TIMEOUT)) {
			/* Report it if the client got an error or a read timeout expired */
			req->analysers = 0;
			if (req->flags & CF_READ_ERROR) {
				HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLICL;
			}
			else if (req->flags & CF_READ_TIMEOUT) {
				HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLITO;
			}
			else if (req->flags & CF_WRITE_ERROR) {
				HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVCL;
			}
			else {
				HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVTO;
			}
			sess_set_term_flags(s);
		}
		else if (res->flags & (CF_READ_ERROR|CF_READ_TIMEOUT|CF_WRITE_ERROR|CF_WRITE_TIMEOUT)) {
			/* Report it if the server got an error or a read timeout expired */
			res->analysers = 0;
			if (res->flags & CF_READ_ERROR) {
				HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVCL;
			}
			else if (res->flags & CF_READ_TIMEOUT) {
				HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVTO;
			}
			else if (res->flags & CF_WRITE_ERROR) {
				HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLICL;
			}
			else {
				HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLITO;
			}
			sess_set_term_flags(s);
		}
	}

	/*
	 * Here we take care of forwarding unhandled data. This also includes
	 * connection establishments and shutdown requests.
	 */


	/* If noone is interested in analysing data, it's time to forward
	 * everything. We configure the buffer to forward indefinitely.
	 * Note that we're checking CF_SHUTR_NOW as an indication of a possible
	 * recent call to channel_abort().
	 */
	if (unlikely((!req->analysers || (req->analysers == AN_REQ_FLT_END && !(req->flags & CF_FLT_ANALYZE))) &&
	    !(req->flags & (CF_SHUTW|CF_SHUTR_NOW)) &&
	    (si_f->state >= SI_ST_EST) &&
	    (req->to_forward != CHN_INFINITE_FORWARD))) {
		/* This buffer is freewheeling, there's no analyser
		 * attached to it. If any data are left in, we'll permit them to
		 * move.
		 */
		channel_auto_read(req);
		channel_auto_connect(req);
		channel_auto_close(req);

		if (IS_HTX_STRM(s)) {
			struct htx *htx = htxbuf(&req->buf);

			/* We'll let data flow between the producer (if still connected)
			 * to the consumer.
			 */
			co_set_data(req, htx->data);
			if (!(req->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_htx_forward_forever(req, htx);
		}
		else {
			/* We'll let data flow between the producer (if still connected)
			 * to the consumer (which might possibly not be connected yet).
			 */
			c_adv(req, ci_data(req));
			if (!(req->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_forward_forever(req);

			/* Just in order to support fetching HTTP contents after start
			 * of forwarding when the HTTP forwarding analyser is not used,
			 * we simply reset msg->sov so that HTTP rewinding points to the
			 * headers.
			 */
			if (s->txn)
				s->txn->req.sov = s->txn->req.eoh + s->txn->req.eol - co_data(req);
		}
	}

	/* check if it is wise to enable kernel splicing to forward request data */
	if (!(req->flags & (CF_KERN_SPLICING|CF_SHUTR)) &&
	    req->to_forward &&
	    (global.tune.options & GTUNE_USE_SPLICE) &&
	    (objt_cs(si_f->end) && __objt_cs(si_f->end)->conn->xprt && __objt_cs(si_f->end)->conn->xprt->rcv_pipe) &&
	    (objt_cs(si_b->end) && __objt_cs(si_b->end)->conn->xprt && __objt_cs(si_b->end)->conn->xprt->snd_pipe) &&
	    (pipes_used < global.maxpipes) &&
	    (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_REQ) ||
	     (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_AUT) &&
	      (req->flags & CF_STREAMER_FAST)))) {
		req->flags |= CF_KERN_SPLICING;
	}

	/* reflect what the L7 analysers have seen last */
	rqf_last = req->flags;

	/*
	 * Now forward all shutdown requests between both sides of the buffer
	 */

	/* first, let's check if the request buffer needs to shutdown(write), which may
	 * happen either because the input is closed or because we want to force a close
	 * once the server has begun to respond. If a half-closed timeout is set, we adjust
	 * the other side's timeout as well.
	 */
	if (unlikely((req->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_AUTO_CLOSE|CF_SHUTR)) ==
		     (CF_AUTO_CLOSE|CF_SHUTR))) {
		channel_shutw_now(req);
	}

	/* shutdown(write) pending */
	if (unlikely((req->flags & (CF_SHUTW|CF_SHUTW_NOW)) == CF_SHUTW_NOW &&
		     channel_is_empty(req))) {
		if (req->flags & CF_READ_ERROR)
			si_b->flags |= SI_FL_NOLINGER;
		si_shutw(si_b);
	}

	/* shutdown(write) done on server side, we must stop the client too */
	if (unlikely((req->flags & (CF_SHUTW|CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTW &&
		     !req->analysers))
		channel_shutr_now(req);

	/* shutdown(read) pending */
	if (unlikely((req->flags & (CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTR_NOW)) {
		if (si_f->flags & SI_FL_NOHALF)
			si_f->flags |= SI_FL_NOLINGER;
		si_shutr(si_f);
	}

	/* it's possible that an upper layer has requested a connection setup or abort.
	 * There are 2 situations where we decide to establish a new connection :
	 *  - there are data scheduled for emission in the buffer
	 *  - the CF_AUTO_CONNECT flag is set (active connection)
	 */
	if (si_b->state == SI_ST_INI) {
		if (!(req->flags & CF_SHUTW)) {
			if ((req->flags & CF_AUTO_CONNECT) || !channel_is_empty(req)) {
				/* If we have an appctx, there is no connect method, so we
				 * immediately switch to the connected state, otherwise we
				 * perform a connection request.
				 */
				si_b->state = SI_ST_REQ; /* new connection requested */
				si_b->conn_retries = s->be->conn_retries;
			}
		}
		else {
			si_release_endpoint(si_b);
			si_b->state = SI_ST_CLO; /* shutw+ini = abort */
			channel_shutw_now(req);        /* fix buffer flags upon abort */
			channel_shutr_now(res);
		}
	}


	/* we may have a pending connection request, or a connection waiting
	 * for completion.
	 */
	if (si_b->state >= SI_ST_REQ && si_b->state < SI_ST_CON) {

		/* prune the request variables and swap to the response variables. */
		if (s->vars_reqres.scope != SCOPE_RES) {
			if (!LIST_ISEMPTY(&s->vars_reqres.head)) {
				vars_prune(&s->vars_reqres, s->sess, s);
				vars_init(&s->vars_reqres, SCOPE_RES);
			}
		}

		do {
			/* nb: step 1 might switch from QUE to ASS, but we first want
			 * to give a chance to step 2 to perform a redirect if needed.
			 */
			if (si_b->state != SI_ST_REQ)
				sess_update_stream_int(s);
			if (si_b->state == SI_ST_REQ)
				sess_prepare_conn_req(s);

			/* applets directly go to the ESTABLISHED state. Similarly,
			 * servers experience the same fate when their connection
			 * is reused.
			 */
			if (unlikely(si_b->state == SI_ST_EST))
				sess_establish(s);

			/* Now we can add the server name to a header (if requested) */
			/* check for HTTP mode and proxy server_name_hdr_name != NULL */
			if ((si_b->state >= SI_ST_CON) && (si_b->state < SI_ST_CLO) &&
			    (s->be->server_id_hdr_name != NULL) &&
			    (s->be->mode == PR_MODE_HTTP) &&
			    objt_server(s->target)) {
				http_send_name_header(s, s->be, objt_server(s->target)->id);
			}

			srv = objt_server(s->target);
			if (si_b->state == SI_ST_ASS && srv && srv->rdr_len && (s->flags & SF_REDIRECTABLE))
				http_perform_server_redirect(s, si_b);
		} while (si_b->state == SI_ST_ASS);
	}

	/* Benchmarks have shown that it's optimal to do a full resync now */
	if (si_f->state == SI_ST_DIS || si_b->state == SI_ST_DIS)
		goto resync_stream_interface;

	/* otherwise we want to check if we need to resync the req buffer or not */
	if ((req->flags ^ rqf_last) & CF_MASK_STATIC)
		goto resync_request;

	/* perform output updates to the response buffer */

	/* If noone is interested in analysing data, it's time to forward
	 * everything. We configure the buffer to forward indefinitely.
	 * Note that we're checking CF_SHUTR_NOW as an indication of a possible
	 * recent call to channel_abort().
	 */
	if (unlikely((!res->analysers || (res->analysers == AN_RES_FLT_END && !(res->flags & CF_FLT_ANALYZE))) &&
	    !(res->flags & (CF_SHUTW|CF_SHUTR_NOW)) &&
	    (si_b->state >= SI_ST_EST) &&
	    (res->to_forward != CHN_INFINITE_FORWARD))) {
		/* This buffer is freewheeling, there's no analyser
		 * attached to it. If any data are left in, we'll permit them to
		 * move.
		 */
		channel_auto_read(res);
		channel_auto_close(res);

		if (IS_HTX_STRM(s)) {
			struct htx *htx = htxbuf(&res->buf);

			/* We'll let data flow between the producer (if still connected)
			 * to the consumer.
			 */
			co_set_data(res, htx->data);
			if (!(res->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_htx_forward_forever(res, htx);
		}
		else {
			/* We'll let data flow between the producer (if still connected)
			 * to the consumer.
			 */
			c_adv(res, ci_data(res));
			if (!(res->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_forward_forever(res);

			/* Just in order to support fetching HTTP contents after start
			 * of forwarding when the HTTP forwarding analyser is not used,
			 * we simply reset msg->sov so that HTTP rewinding points to the
			 * headers.
			 */
			if (s->txn)
				s->txn->rsp.sov = s->txn->rsp.eoh + s->txn->rsp.eol - co_data(res);
		}

		/* if we have no analyser anymore in any direction and have a
		 * tunnel timeout set, use it now. Note that we must respect
		 * the half-closed timeouts as well.
		 */
		if (!req->analysers && s->be->timeout.tunnel) {
			req->rto = req->wto = res->rto = res->wto =
				s->be->timeout.tunnel;

			if ((req->flags & CF_SHUTR) && tick_isset(sess->fe->timeout.clientfin))
				res->wto = sess->fe->timeout.clientfin;
			if ((req->flags & CF_SHUTW) && tick_isset(s->be->timeout.serverfin))
				res->rto = s->be->timeout.serverfin;
			if ((res->flags & CF_SHUTR) && tick_isset(s->be->timeout.serverfin))
				req->wto = s->be->timeout.serverfin;
			if ((res->flags & CF_SHUTW) && tick_isset(sess->fe->timeout.clientfin))
				req->rto = sess->fe->timeout.clientfin;

			req->rex = tick_add(now_ms, req->rto);
			req->wex = tick_add(now_ms, req->wto);
			res->rex = tick_add(now_ms, res->rto);
			res->wex = tick_add(now_ms, res->wto);
		}
	}

	/* check if it is wise to enable kernel splicing to forward response data */
	if (!(res->flags & (CF_KERN_SPLICING|CF_SHUTR)) &&
	    res->to_forward &&
	    (global.tune.options & GTUNE_USE_SPLICE) &&
	    (objt_cs(si_f->end) && __objt_cs(si_f->end)->conn->xprt && __objt_cs(si_f->end)->conn->xprt->snd_pipe) &&
	    (objt_cs(si_b->end) && __objt_cs(si_b->end)->conn->xprt && __objt_cs(si_b->end)->conn->xprt->rcv_pipe) &&
	    (pipes_used < global.maxpipes) &&
	    (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_RTR) ||
	     (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_AUT) &&
	      (res->flags & CF_STREAMER_FAST)))) {
		res->flags |= CF_KERN_SPLICING;
	}

	/* reflect what the L7 analysers have seen last */
	rpf_last = res->flags;

	/*
	 * Now forward all shutdown requests between both sides of the buffer
	 */

	/*
	 * FIXME: this is probably where we should produce error responses.
	 */

	/* first, let's check if the response buffer needs to shutdown(write) */
	if (unlikely((res->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_AUTO_CLOSE|CF_SHUTR)) ==
		     (CF_AUTO_CLOSE|CF_SHUTR))) {
		channel_shutw_now(res);
	}

	/* shutdown(write) pending */
	if (unlikely((res->flags & (CF_SHUTW|CF_SHUTW_NOW)) == CF_SHUTW_NOW &&
		     channel_is_empty(res))) {
		si_shutw(si_f);
	}

	/* shutdown(write) done on the client side, we must stop the server too */
	if (unlikely((res->flags & (CF_SHUTW|CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTW) &&
	    !res->analysers)
		channel_shutr_now(res);

	/* shutdown(read) pending */
	if (unlikely((res->flags & (CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTR_NOW)) {
		if (si_b->flags & SI_FL_NOHALF)
			si_b->flags |= SI_FL_NOLINGER;
		si_shutr(si_b);
	}

	if (si_f->state == SI_ST_DIS || si_b->state == SI_ST_DIS)
		goto resync_stream_interface;

	if (req->flags != rqf_last)
		goto resync_request;

	if ((res->flags ^ rpf_last) & CF_MASK_STATIC)
		goto resync_response;

	/* we're interested in getting wakeups again */
	si_f->flags &= ~SI_FL_DONT_WAKE;
	si_b->flags &= ~SI_FL_DONT_WAKE;

	/* This is needed only when debugging is enabled, to indicate
	 * client-side or server-side close. Please note that in the unlikely
	 * event where both sides would close at once, the sequence is reported
	 * on the server side first.
	 */
	if (unlikely((global.mode & MODE_DEBUG) &&
		     (!(global.mode & MODE_QUIET) ||
		      (global.mode & MODE_VERBOSE)))) {
		if (si_b->state == SI_ST_CLO &&
		    si_b->prev_state == SI_ST_EST) {
			chunk_printf(&trash, "%08x:%s.srvcls[%04x:%04x]\n",
				      s->uniq_id, s->be->id,
			              objt_cs(si_f->end) ? (unsigned short)objt_cs(si_f->end)->conn->handle.fd : -1,
			              objt_cs(si_b->end) ? (unsigned short)objt_cs(si_b->end)->conn->handle.fd : -1);
			shut_your_big_mouth_gcc(write(1, trash.area, trash.data));
		}

		if (si_f->state == SI_ST_CLO &&
		    si_f->prev_state == SI_ST_EST) {
			chunk_printf(&trash, "%08x:%s.clicls[%04x:%04x]\n",
				      s->uniq_id, s->be->id,
			              objt_cs(si_f->end) ? (unsigned short)objt_cs(si_f->end)->conn->handle.fd : -1,
			              objt_cs(si_b->end) ? (unsigned short)objt_cs(si_b->end)->conn->handle.fd : -1);
			shut_your_big_mouth_gcc(write(1, trash.area, trash.data));
		}
	}

	if (likely((si_f->state != SI_ST_CLO) ||
		   (si_b->state > SI_ST_INI && si_b->state < SI_ST_CLO))) {

		if ((sess->fe->options & PR_O_CONTSTATS) && (s->flags & SF_BE_ASSIGNED))
			stream_process_counters(s);

		si_update_both(si_f, si_b);

		if (si_f->state == SI_ST_DIS || si_f->state != si_f->prev_state ||
		    si_b->state == SI_ST_DIS || si_b->state != si_b->prev_state ||
		    (((req->flags ^ rqf_last) | (res->flags ^ rpf_last)) & CF_MASK_ANALYSER))
			goto redo;

		/* Trick: if a request is being waiting for the server to respond,
		 * and if we know the server can timeout, we don't want the timeout
		 * to expire on the client side first, but we're still interested
		 * in passing data from the client to the server (eg: POST). Thus,
		 * we can cancel the client's request timeout if the server's
		 * request timeout is set and the server has not yet sent a response.
		 */

		if ((res->flags & (CF_AUTO_CLOSE|CF_SHUTR)) == 0 &&
		    (tick_isset(req->wex) || tick_isset(res->rex))) {
			req->flags |= CF_READ_NOEXP;
			req->rex = TICK_ETERNITY;
		}

		/* Reset pending events now */
		s->pending_events = 0;

	update_exp_and_leave:
		/* Note: please ensure that if you branch here you disable SI_FL_DONT_WAKE */
		t->expire = tick_first((tick_is_expired(t->expire, now_ms) ? 0 : t->expire),
				       tick_first(tick_first(req->rex, req->wex),
						  tick_first(res->rex, res->wex)));
		if (!req->analysers)
			req->analyse_exp = TICK_ETERNITY;

		if ((sess->fe->options & PR_O_CONTSTATS) && (s->flags & SF_BE_ASSIGNED) &&
		          (!tick_isset(req->analyse_exp) || tick_is_expired(req->analyse_exp, now_ms)))
			req->analyse_exp = tick_add(now_ms, 5000);

		t->expire = tick_first(t->expire, req->analyse_exp);

		t->expire = tick_first(t->expire, res->analyse_exp);

		if (si_f->exp)
			t->expire = tick_first(t->expire, si_f->exp);

		if (si_b->exp)
			t->expire = tick_first(t->expire, si_b->exp);

		DPRINTF(stderr,
			"[%u] queuing with exp=%u req->rex=%u req->wex=%u req->ana_exp=%u"
			" rep->rex=%u rep->wex=%u, si[0].exp=%u, si[1].exp=%u, cs=%d, ss=%d\n",
			now_ms, t->expire, req->rex, req->wex, req->analyse_exp,
			res->rex, res->wex, si_f->exp, si_b->exp, si_f->state, si_b->state);

#ifdef DEBUG_DEV
		/* this may only happen when no timeout is set or in case of an FSM bug */
		if (!tick_isset(t->expire))
			ABORT_NOW();
#endif
		s->pending_events &= ~(TASK_WOKEN_TIMER | TASK_WOKEN_RES);
		stream_release_buffers(s);
		/* We may have free'd some space in buffers, or have more to send/recv, try again */
		return t; /* nothing more to do */
	}

	if (s->flags & SF_BE_ASSIGNED)
		HA_ATOMIC_SUB(&s->be->beconn, 1);

	if (unlikely((global.mode & MODE_DEBUG) &&
		     (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)))) {
		chunk_printf(&trash, "%08x:%s.closed[%04x:%04x]\n",
			      s->uniq_id, s->be->id,
		              objt_cs(si_f->end) ? (unsigned short)objt_cs(si_f->end)->conn->handle.fd : -1,
		              objt_cs(si_b->end) ? (unsigned short)objt_cs(si_b->end)->conn->handle.fd : -1);
		shut_your_big_mouth_gcc(write(1, trash.area, trash.data));
	}

	s->logs.t_close = tv_ms_elapsed(&s->logs.tv_accept, &now);
	stream_process_counters(s);

	if (s->txn && s->txn->status) {
		int n;

		n = s->txn->status / 100;
		if (n < 1 || n > 5)
			n = 0;

		if (sess->fe->mode == PR_MODE_HTTP) {
			HA_ATOMIC_ADD(&sess->fe->fe_counters.p.http.rsp[n], 1);
		}
		if ((s->flags & SF_BE_ASSIGNED) &&
		    (s->be->mode == PR_MODE_HTTP)) {
			HA_ATOMIC_ADD(&s->be->be_counters.p.http.rsp[n], 1);
			HA_ATOMIC_ADD(&s->be->be_counters.p.http.cum_req, 1);
		}
	}

	/* let's do a final log if we need it */
	if (!LIST_ISEMPTY(&sess->fe->logformat) && s->logs.logwait &&
	    !(s->flags & SF_MONITOR) &&
	    (!(sess->fe->options & PR_O_NULLNOLOG) || req->total)) {
		/* we may need to know the position in the queue */
		pendconn_free(s);
		s->do_log(s);
	}

	/* update time stats for this stream */
	stream_update_time_stats(s);

	/* the task MUST not be in the run queue anymore */
	stream_free(s);
	task_delete(t);
	task_free(t);
	return NULL;
}
#endif

#if 0
	conn_refresh_polling_flags(conn);
	conn->flags |= CO_FL_WILL_UPDATE;

	flags = conn->flags & ~CO_FL_ERROR; /* ensure to call the wake handler upon error */

 process_handshake:
	/* The handshake callbacks are called in sequence. If either of them is
	 * missing something, it must enable the required polling at the socket
	 * layer of the connection. Polling state is not guaranteed when entering
	 * these handlers, so any handshake handler which does not complete its
	 * work must explicitly disable events it's not interested in. Error
	 * handling is also performed here in order to reduce the number of tests
	 * around.
	 */
	while (unlikely(conn->flags & (CO_FL_HANDSHAKE | CO_FL_ERROR))) {
		if (unlikely(conn->flags & CO_FL_ERROR))
			goto leave;

		if (conn->flags & CO_FL_ACCEPT_CIP)
			if (!conn_recv_netscaler_cip(conn, CO_FL_ACCEPT_CIP))
				goto leave;

		if (conn->flags & CO_FL_ACCEPT_PROXY)
			if (!conn_recv_proxy(conn, CO_FL_ACCEPT_PROXY))
				goto leave;

		if (conn->flags & CO_FL_SEND_PROXY)
			if (!conn_si_send_proxy(conn, CO_FL_SEND_PROXY))
				goto leave;
#ifdef USE_OPENSSL
		if (conn->flags & CO_FL_SSL_WAIT_HS)
			if (!ssl_sock_handshake(conn, CO_FL_SSL_WAIT_HS))
				goto leave;
#endif
	}

	/* Once we're purely in the data phase, we disable handshake polling */
	if (!(conn->flags & CO_FL_POLL_SOCK))
		__conn_sock_stop_both(conn);

	/* The connection owner might want to be notified about an end of
	 * handshake indicating the connection is ready, before we proceed with
	 * any data exchange. The callback may fail and cause the connection to
	 * be destroyed, thus we must not use it anymore and should immediately
	 * leave instead. The caller must immediately unregister itself once
	 * called.
	 */
	if (conn->xprt_done_cb && conn->xprt_done_cb(conn) < 0)
		return;
		
	if (conn->xprt && fd_send_ready(fd) &&
		((conn->flags & (CO_FL_XPRT_WR_ENA|CO_FL_ERROR|CO_FL_HANDSHAKE)) == CO_FL_XPRT_WR_ENA)) {
		/* force reporting of activity by clearing the previous flags :
		 * we'll have at least ERROR or CONNECTED at the end of an I/O,
		 * both of which will be detected below.
		 */
		flags = 0;
		if (conn->send_wait != NULL) {
			conn->send_wait->events &= ~SUB_RETRY_SEND;
			tasklet_wakeup(conn->send_wait->task);
			conn->send_wait = NULL;
		} else
			io_available = 1;
		__conn_xprt_stop_send(conn);
	}

	/* The data transfer starts here and stops on error and handshakes. Note
	 * that we must absolutely test conn->xprt at each step in case it suddenly
	 * changes due to a quick unexpected close().
	 */
	if (conn->xprt && fd_recv_ready(fd) &&
	    ((conn->flags & (CO_FL_XPRT_RD_ENA|CO_FL_WAIT_ROOM|CO_FL_ERROR|CO_FL_HANDSHAKE)) == CO_FL_XPRT_RD_ENA)) {
		/* force reporting of activity by clearing the previous flags :
		 * we'll have at least ERROR or CONNECTED at the end of an I/O,
		 * both of which will be detected below.
		 */
		flags = 0;
		if (conn->recv_wait) {
			conn->recv_wait->events &= ~SUB_RETRY_RECV;
			tasklet_wakeup(conn->recv_wait->task);
			conn->recv_wait = NULL;
		} else
			io_available = 1;
		__conn_xprt_stop_recv(conn);
	}

	/* It may happen during the data phase that a handshake is
	 * enabled again (eg: SSL)
	 */
	if (unlikely(conn->flags & (CO_FL_HANDSHAKE | CO_FL_ERROR)))
		goto process_handshake;

	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN)) {
		/* still waiting for a connection to establish and nothing was
		 * attempted yet to probe the connection. Then let's retry the
		 * connect().
		 */
		if (!tcp_connect_probe(conn))
			goto leave;
	}
 leave:
	/* Verify if the connection just established. */
	if (unlikely(!(conn->flags & (CO_FL_WAIT_L4_CONN | CO_FL_WAIT_L6_CONN | CO_FL_CONNECTED))))
		conn->flags |= CO_FL_CONNECTED;

	/* The connection owner might want to be notified about failures to
	 * complete the handshake. The callback may fail and cause the
	 * connection to be destroyed, thus we must not use it anymore and
	 * should immediately leave instead. The caller must immediately
	 * unregister itself once called.
	 */
	if (((conn->flags ^ flags) & CO_FL_NOTIFY_DONE) &&
	    conn->xprt_done_cb && conn->xprt_done_cb(conn) < 0)
		return;

	/* The wake callback is normally used to notify the data layer about
	 * data layer activity (successful send/recv), connection establishment,
	 * shutdown and fatal errors. We need to consider the following
	 * situations to wake up the data layer :
	 *  - change among the CO_FL_NOTIFY_DATA flags :
	 *      {DATA,SOCK}_{RD,WR}_SH, ERROR,
	 *  - absence of any of {L4,L6}_CONN and CONNECTED, indicating the
	 *    end of handshake and transition to CONNECTED
	 *  - raise of CONNECTED with HANDSHAKE down
	 *  - end of HANDSHAKE with CONNECTED set
	 *  - regular data layer activity
	 *
	 * Note that the wake callback is allowed to release the connection and
	 * the fd (and return < 0 in this case).
	 */
	if ((io_available || (((conn->flags ^ flags) & CO_FL_NOTIFY_DATA) ||
	     ((flags & (CO_FL_CONNECTED|CO_FL_HANDSHAKE)) != CO_FL_CONNECTED &&
	      (conn->flags & (CO_FL_CONNECTED|CO_FL_HANDSHAKE)) == CO_FL_CONNECTED))) &&
	    conn->mux->wake && conn->mux->wake(conn) < 0)
		return;

	/* commit polling changes */
	conn->flags &= ~CO_FL_WILL_UPDATE;
	conn_cond_update_polling(conn);
#endif


#endif
