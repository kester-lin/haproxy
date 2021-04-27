/*
 * Functions managing stream_interface structures
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/buffer.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>

#include <proto/applet.h>
#include <proto/channel.h>
#include <proto/connection.h>
#include <proto/http_htx.h>
#include <proto/mux_pt.h>
#include <proto/pipe.h>
#include <proto/proxy.h>
#include <proto/stream.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

#include <types/global.h>
#include <types/pipe.h>

unsigned long long g_out_data = 0;
unsigned long long g_in_data = 0;

#define DEBUG_SI_RECV 0
#if DEBUG_SI_RECV
#define SIRPRINTF(x...) printf(x)
#else
#define SIRPRINTF(x...)
#endif


#define DEBUG_SI_SEND 0
#if DEBUG_SI_SEND
#define SISPRINTF(x...) printf(x)
#else
#define SISPRINTF(x...)
#endif

/* functions used by default on a detached stream-interface */
static void stream_int_shutr(struct stream_interface *si);
static void stream_int_shutw(struct stream_interface *si);
static void stream_int_chk_rcv(struct stream_interface *si);
static void stream_int_chk_snd(struct stream_interface *si);

/* functions used on a conn_stream-based stream-interface */
static void stream_int_shutr_conn(struct stream_interface *si);
static void stream_int_shutw_conn(struct stream_interface *si);
static void stream_int_chk_rcv_conn(struct stream_interface *si);
static void stream_int_chk_snd_conn(struct stream_interface *si);

/* functions used on an applet-based stream-interface */
static void stream_int_shutr_applet(struct stream_interface *si);
static void stream_int_shutw_applet(struct stream_interface *si);
static void stream_int_chk_rcv_applet(struct stream_interface *si);
static void stream_int_chk_snd_applet(struct stream_interface *si);

/* last read notification */
static void stream_int_read0(struct stream_interface *si);

/* post-IO notification callback */
static void stream_int_notify(struct stream_interface *si);

/* stream-interface operations for embedded tasks */
struct si_ops si_embedded_ops = {
	.chk_rcv = stream_int_chk_rcv,
	.chk_snd = stream_int_chk_snd,
	.shutr   = stream_int_shutr,
	.shutw   = stream_int_shutw,
};

/* stream-interface operations for connections */
struct si_ops si_conn_ops = {
	.chk_rcv = stream_int_chk_rcv_conn,
	.chk_snd = stream_int_chk_snd_conn,
	.shutr   = stream_int_shutr_conn,
	.shutw   = stream_int_shutw_conn,
};

/* stream-interface operations for connections */
struct si_ops si_applet_ops = {
	.chk_rcv = stream_int_chk_rcv_applet,
	.chk_snd = stream_int_chk_snd_applet,
	.shutr   = stream_int_shutr_applet,
	.shutw   = stream_int_shutw_applet,
};


/* Functions used to communicate with a conn_stream. The first two may be used
 * directly, the last one is mostly a wake callback.
 */
int si_cs_recv(struct conn_stream *cs);
int si_cs_send(struct conn_stream *cs);
static int si_cs_process(struct conn_stream *cs);


struct data_cb si_conn_cb = {
	.wake    = si_cs_process,
	.name    = "STRM",
};

/*
 * This function only has to be called once after a wakeup event in case of
 * suspected timeout. It controls the stream interface timeouts and sets
 * si->flags accordingly. It does NOT close anything, as this timeout may
 * be used for any purpose. It returns 1 if the timeout fired, otherwise
 * zero.
 */
int si_check_timeouts(struct stream_interface *si)
{
	SIPRINTF("[%s]\n", __func__);

	if (tick_is_expired(si->exp, now_ms)) {
		si->flags |= SI_FL_EXP;
		return 1;
	}
	return 0;
}

/* to be called only when in SI_ST_DIS with SI_FL_ERR */
void si_report_error(struct stream_interface *si)
{
	SIPRINTF("[%s]\n", __func__);

	if (!si->err_type)
		si->err_type = SI_ET_DATA_ERR;

	si_oc(si)->flags |= CF_WRITE_ERROR;
	si_ic(si)->flags |= CF_READ_ERROR;
}

/*
 * Returns a message to the client ; the connection is shut down for read,
 * and the request is cleared so that no server connection can be initiated.
 * The buffer is marked for read shutdown on the other side to protect the
 * message, and the buffer write is enabled. The message is contained in a
 * "chunk". If it is null, then an empty message is used. The reply buffer does
 * not need to be empty before this, and its contents will not be overwritten.
 * The primary goal of this function is to return error messages to a client.
 */
void si_retnclose(struct stream_interface *si,
			  const struct buffer *msg)
{
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	channel_auto_read(ic);
	channel_abort(ic);
	channel_auto_close(ic);
	channel_erase(ic);
	channel_truncate(oc);

	if (likely(msg && msg->data))
		co_inject(oc, msg->area, msg->data);

	oc->wex = tick_add_ifset(now_ms, oc->wto);
	channel_auto_read(oc);
	channel_auto_close(oc);
	channel_shutr_now(oc);
}

/*
 * This function performs a shutdown-read on a detached stream interface in a
 * connected or init state (it does nothing for other states). It either shuts
 * the read side or marks itself as closed. The buffer flags are updated to
 * reflect the new state. If the stream interface has SI_FL_NOHALF, we also
 * forward the close to the write side. The owner task is woken up if it exists.
 */
static void stream_int_shutr(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	si_rx_shut_blk(si);
	if (ic->flags & CF_SHUTR)
		return;
	ic->flags |= CF_SHUTR;
	ic->rex = TICK_ETERNITY;

	if (!si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST))
		return;

	if (si_oc(si)->flags & CF_SHUTW) {
		si->state = SI_ST_DIS;
		si->exp = TICK_ETERNITY;
	}
	else if (si->flags & SI_FL_NOHALF) {
		/* we want to immediately forward this close to the write side */
		return stream_int_shutw(si);
	}

	/* note that if the task exists, it must unregister itself once it runs */
	if (!(si->flags & SI_FL_DONT_WAKE))
		task_wakeup(si_task(si), TASK_WOKEN_IO);
}

/*
 * This function performs a shutdown-write on a detached stream interface in a
 * connected or init state (it does nothing for other states). It either shuts
 * the write side or marks itself as closed. The buffer flags are updated to
 * reflect the new state. It does also close everything if the SI was marked as
 * being in error state. The owner task is woken up if it exists.
 */
static void stream_int_shutw(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	oc->flags &= ~CF_SHUTW_NOW;
	if (oc->flags & CF_SHUTW)
		return;
	oc->flags |= CF_SHUTW;
	oc->wex = TICK_ETERNITY;
	si_done_get(si);

	if (tick_isset(si->hcto)) {
		ic->rto = si->hcto;
		ic->rex = tick_add(now_ms, ic->rto);
	}

	switch (si->state) {
	case SI_ST_RDY:
	case SI_ST_EST:
		/* we have to shut before closing, otherwise some short messages
		 * may never leave the system, especially when there are remaining
		 * unread data in the socket input buffer, or when nolinger is set.
		 * However, if SI_FL_NOLINGER is explicitly set, we know there is
		 * no risk so we close both sides immediately.
		 */
		if (!(si->flags & (SI_FL_ERR | SI_FL_NOLINGER)) &&
		    !(ic->flags & (CF_SHUTR|CF_DONT_READ)))
			return;

		/* fall through */
	case SI_ST_CON:
	case SI_ST_CER:
	case SI_ST_QUE:
	case SI_ST_TAR:
		/* Note that none of these states may happen with applets */
		si->state = SI_ST_DIS;
	default:
		si->flags &= ~SI_FL_NOLINGER;
		si_rx_shut_blk(si);
		ic->flags |= CF_SHUTR;
		ic->rex = TICK_ETERNITY;
		si->exp = TICK_ETERNITY;
	}

	/* note that if the task exists, it must unregister itself once it runs */
	if (!(si->flags & SI_FL_DONT_WAKE))
		task_wakeup(si_task(si), TASK_WOKEN_IO);
}

/* default chk_rcv function for scheduled tasks */
static void stream_int_chk_rcv(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	DPRINTF(stderr, "%s: si=%p, si->state=%d ic->flags=%08x oc->flags=%08x\n",
		__FUNCTION__,
		si, si->state, ic->flags, si_oc(si)->flags);

	if (ic->pipe) {
		/* stop reading */
		si_rx_room_blk(si);
	}
	else {
		/* (re)start reading */
		tasklet_wakeup(si->wait_event.tasklet);
		if (!(si->flags & SI_FL_DONT_WAKE))
			task_wakeup(si_task(si), TASK_WOKEN_IO);
	}
}

/* default chk_snd function for scheduled tasks */
static void stream_int_chk_snd(struct stream_interface *si)
{
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	DPRINTF(stderr, "%s: si=%p, si->state=%d ic->flags=%08x oc->flags=%08x\n",
		__FUNCTION__,
		si, si->state, si_ic(si)->flags, oc->flags);

	if (unlikely(si->state != SI_ST_EST || (oc->flags & CF_SHUTW)))
		return;

	if (!(si->flags & SI_FL_WAIT_DATA) ||        /* not waiting for data */
	    channel_is_empty(oc))           /* called with nothing to send ! */
		return;

	/* Otherwise there are remaining data to be sent in the buffer,
	 * so we tell the handler.
	 */
	si->flags &= ~SI_FL_WAIT_DATA;
	if (!tick_isset(oc->wex))
		oc->wex = tick_add_ifset(now_ms, oc->wto);

	if (!(si->flags & SI_FL_DONT_WAKE))
		task_wakeup(si_task(si), TASK_WOKEN_IO);
}

/* Register an applet to handle a stream_interface as a new appctx. The SI will
 * wake it up everytime it is solicited. The appctx must be deleted by the task
 * handler using si_release_endpoint(), possibly from within the function itself.
 * It also pre-initializes the applet's context and returns it (or NULL in case
 * it could not be allocated).
 */
struct appctx *si_register_handler(struct stream_interface *si, struct applet *app)
{
	struct appctx *appctx;

	SIPRINTF("[%s]\n", __func__);

	DPRINTF(stderr, "registering handler %p for si %p (was %p)\n", app, si, si_task(si));

	appctx = si_alloc_appctx(si, app);
	if (!appctx)
		return NULL;

	si_cant_get(si);
	appctx_wakeup(appctx);
	return si_appctx(si);
}

/* This callback is used to send a valid PROXY protocol line to a socket being
 * established. It returns 0 if it fails in a fatal way or needs to poll to go
 * further, otherwise it returns non-zero and removes itself from the connection's
 * flags (the bit is provided in <flag> by the caller). It is designed to be
 * called by the connection handler and relies on it to commit polling changes.
 * Note that it can emit a PROXY line by relying on the other end's address
 * when the connection is attached to a stream interface, or by resolving the
 * local address otherwise (also called a LOCAL line).
 */
int conn_si_send_proxy(struct connection *conn, unsigned int flag)
{
	SIPRINTF("[%s]\n", __func__);

	/* we might have been called just after an asynchronous shutw */
	if (conn->flags & CO_FL_SOCK_WR_SH)
		goto out_error;

	if (!conn_ctrl_ready(conn))
		goto out_error;

	/* If we have a PROXY line to send, we'll use this to validate the
	 * connection, in which case the connection is validated only once
	 * we've sent the whole proxy line. Otherwise we use connect().
	 */
	if (conn->send_proxy_ofs) {
		const struct conn_stream *cs;
		int ret;

		cs = cs_get_first(conn);
		/* The target server expects a PROXY line to be sent first.
		 * If the send_proxy_ofs is negative, it corresponds to the
		 * offset to start sending from then end of the proxy string
		 * (which is recomputed every time since it's constant). If
		 * it is positive, it means we have to send from the start.
		 * We can only send a "normal" PROXY line when the connection
		 * is attached to a stream interface. Otherwise we can only
		 * send a LOCAL line (eg: for use with health checks).
		 */

		if (cs && cs->data_cb == &si_conn_cb) {
			struct stream_interface *si = cs->data;
			struct conn_stream *remote_cs = objt_cs(si_opposite(si)->end);
			ret = make_proxy_line(trash.area, trash.size,
					      objt_server(conn->target),
					      remote_cs ? remote_cs->conn : NULL);
			/* We may not have a conn_stream yet, if we don't
			 * know which mux to use, because it will be decided
			 * during the SSL handshake. In this case, there should
			 * be a session associated to the connection in
			 * conn->owner, and we know it is the session that
			 * initiated that connection, so we can just use
			 * its origin, which should contain the client
			 * connection.
			 */
		} else if (!cs && conn->owner) {
			struct session *sess = conn->owner;

			ret = make_proxy_line(trash.area, trash.size,
			                      objt_server(conn->target),
					      objt_conn(sess->origin));
		}
		else {
			/* The target server expects a LOCAL line to be sent first. Retrieving
			 * local or remote addresses may fail until the connection is established.
			 */
			conn_get_from_addr(conn);
			if (!(conn->flags & CO_FL_ADDR_FROM_SET))
				goto out_wait;

			conn_get_to_addr(conn);
			if (!(conn->flags & CO_FL_ADDR_TO_SET))
				goto out_wait;

			ret = make_proxy_line(trash.area, trash.size,
					      objt_server(conn->target), conn);
		}

		if (!ret)
			goto out_error;

		if (conn->send_proxy_ofs > 0)
			conn->send_proxy_ofs = -ret; /* first call */

		/* we have to send trash from (ret+sp for -sp bytes). If the
		 * data layer has a pending write, we'll also set MSG_MORE.
		 */
		ret = conn_sock_send(conn,
				     trash.area + ret + conn->send_proxy_ofs,
		                     -conn->send_proxy_ofs,
		                     (conn->flags & CO_FL_XPRT_WR_ENA) ? MSG_MORE : 0);

		if (ret < 0)
			goto out_error;

		conn->send_proxy_ofs += ret; /* becomes zero once complete */
		if (conn->send_proxy_ofs != 0)
			goto out_wait;

		/* OK we've sent the whole line, we're connected */
	}

	/* The connection is ready now, simply return and let the connection
	 * handler notify upper layers if needed.
	 */
	if (conn->flags & CO_FL_WAIT_L4_CONN)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;
	conn->flags &= ~flag;
	return 1;

 out_error:
	/* Write error on the file descriptor */
	conn->flags |= CO_FL_ERROR;
	return 0;

 out_wait:
	return 0;
}


/* This function is the equivalent to si_update() except that it's
 * designed to be called from outside the stream handlers, typically the lower
 * layers (applets, connections) after I/O completion. After updating the stream
 * interface and timeouts, it will try to forward what can be forwarded, then to
 * wake the associated task up if an important event requires special handling.
 * It may update SI_FL_WAIT_DATA and/or SI_FL_RXBLK_ROOM, that the callers are
 * encouraged to watch to take appropriate action.
 * It should not be called from within the stream itself, si_update()
 * is designed for this.
 */
static void stream_int_notify(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);
	struct stream_interface *sio = si_opposite(si);
	struct task *task = si_task(si);

	int value_1 = 0;
	int value_2 = 0;
	int value_3 = 0;

	int if_value_1 = 0;
	int if_value_2 = 0;
	int if_value_3 = 0;
	int if_value_4 = 0;
	int if_value_5 = 0;
	int if_value_6 = 0;
	int if_value_7 = 0;
	int if_value_8 = 0;
	int if_value_9 = 0;
	int if_value_10 = 0;
	int if_value_11 = 0;
	int if_value_12 = 0;
	int if_value_13 = 0;

	SIPRINTF("[%s]\n", __func__);

	/* process consumer side */
	if (channel_is_empty(oc)) {
		struct connection *conn = objt_cs(si->end) ? objt_cs(si->end)->conn : NULL;

		if (((oc->flags & (CF_SHUTW|CF_SHUTW_NOW)) == CF_SHUTW_NOW) &&
		    (si->state == SI_ST_EST) && (!conn || !(conn->flags & (CO_FL_HANDSHAKE | CO_FL_EARLY_SSL_HS)))) {
			SIPRINTF("[%s] si_shutw(si)\n", __func__);
			si_shutw(si);
		}
		SIPRINTF("[%s] oc->wex = TICK_ETERNITY\n", __func__);
		oc->wex = TICK_ETERNITY;
	}

	/* indicate that we may be waiting for data from the output channel or
	 * we're about to close and can't expect more data if SHUTW_NOW is there.
	 */
	if (!(oc->flags & (CF_SHUTW|CF_SHUTW_NOW)))
		si->flags |= SI_FL_WAIT_DATA; /* Default Run */
	else if ((oc->flags & (CF_SHUTW|CF_SHUTW_NOW)) == CF_SHUTW_NOW)
		si->flags &= ~SI_FL_WAIT_DATA;

	/* update OC timeouts and wake the other side up if it's waiting for room */
	if (oc->flags & CF_WRITE_ACTIVITY) {
		if ((oc->flags & (CF_SHUTW|CF_WRITE_PARTIAL)) == CF_WRITE_PARTIAL &&
		    !channel_is_empty(oc))
			if (tick_isset(oc->wex))
				oc->wex = tick_add_ifset(now_ms, oc->wto);

		if (!(si->flags & SI_FL_INDEP_STR))
			if (tick_isset(ic->rex))
				ic->rex = tick_add_ifset(now_ms, ic->rto);
	}

	if (oc->flags & CF_DONT_READ)
		si_rx_chan_blk(sio); /* Default not run */
	else
		si_rx_chan_rdy(sio); /* default run */

	/* Notify the other side when we've injected data into the IC that
	 * needs to be forwarded. We can do fast-forwarding as soon as there
	 * are output data, but we avoid doing this if some of the data are
	 * not yet scheduled for being forwarded, because it is very likely
	 * that it will be done again immediately afterwards once the following
	 * data are parsed (eg: HTTP chunking). We only SI_FL_RXBLK_ROOM once
	 * we've emptied *some* of the output buffer, and not just when there
	 * is available room, because applets are often forced to stop before
	 * the buffer is full. We must not stop based on input data alone because
	 * an HTTP parser might need more data to complete the parsing.
	 */  
#if 1
		value_1 = !channel_is_empty(ic);
		value_2 = (sio->flags & SI_FL_WAIT_DATA);
		value_3 = (!(ic->flags & CF_EXPECT_MORE) || c_full(ic) || ci_data(ic) == 0 || ic->pipe);

		SIPRINTF("[%s] Ready Send Value: %d %d %d (%d)\n", __func__, value_1, value_2, value_3, !cont_tx_idx);

		if (value_1 && value_2 && value_3) {	
#else
	if (!channel_is_empty(ic) &&
	    (sio->flags & SI_FL_WAIT_DATA) &&
	    (!(ic->flags & CF_EXPECT_MORE) || c_full(ic) || ci_data(ic) == 0 || ic->pipe)) {
#endif	
		int new_len, last_len;

		last_len = co_data(ic); /* default not run at 1 run at 2 */
		if (ic->pipe)
			last_len += ic->pipe->data;  /* default run */

		SIPRINTF("[%s] si_chk_snd\n", __func__);

		si_chk_snd(sio);

		new_len = co_data(ic);
		if (ic->pipe)
			new_len += ic->pipe->data;

		/* check if the consumer has freed some space either in the
		 * buffer or in the pipe.
		 */
		if (new_len < last_len)
			si_rx_room_rdy(si); /* default run */
	}

	if (!(ic->flags & CF_DONT_READ))
		si_rx_chan_rdy(si);

	si_chk_rcv(si);
	si_chk_rcv(sio);
	
	if (si_rx_blocked(si)) {
		ic->rex = TICK_ETERNITY;  /* default not run  Error at here  */
	}
	else if ((ic->flags & (CF_SHUTR|CF_READ_PARTIAL)) == CF_READ_PARTIAL) { /* default run */
		/* we must re-enable reading if si_chk_snd() has freed some space */
		if (!(ic->flags & CF_READ_NOEXP) && tick_isset(ic->rex))
			ic->rex = tick_add_ifset(now_ms, ic->rto);
	}

#if 0
	SIPRINTF("[%s] now_ms:%16X\n", __func__, now_ms);
	SIPRINTF("[%s] task key:%08X\n", __func__, task->wq.key);
	
	if (cont_tx_idx) {
		#if 0
		channel_dont_read(ic);
		SIPRINTF("[%s] Set task_wakeup\n", __func__);	
		task_wakeup(task, TASK_WOKEN_OTHER);
		#endif
		#if 1
		SIPRINTF("[%s] Set task_queue\n", __func__);
		SIPRINTF("[%s] task expire:%16X\n", __func__, task->expire);
		SIPRINTF("[%s] now_ms:%16X\n", __func__, now_ms);
		SIPRINTF("[%s] ic->rex:%16X\n", __func__, ic->rex);
		SIPRINTF("[%s] ic->wex:%16X\n", __func__, ic->wex);
		SIPRINTF("[%s] oc->rex:%16X\n", __func__, oc->rex);
		SIPRINTF("[%s] oc->wex:%16X\n", __func__, oc->wex);
		SIPRINTF("[%s] task key:%08X\n", __func__, task->wq.key);

		task->expire = tick_first(
					   (tick_is_expired(task->expire, now_ms) ? 0 : task->expire),
						tick_first(tick_first(ic->rex, ic->wex), 
					   			   tick_first(oc->rex, oc->wex))
					   );
		SIPRINTF("[%s] Normal task->expire:%16X\n", __func__, task->expire);	

/*
		if (tick_is_le(task->expire, task->wq.key)) {
			SIPRINTF("[%s] change !!!\n", __func__);
			task->expire = now_ms + 100;
		}
*/
		task->expire = tick_first(task->expire, now_ms) + 10;

		SIPRINTF("[%s] Final task->expire:%16X\n", __func__, task->expire);	

		task_queue(task);
		//__task_queue(task, &task_per_thread[tid].timers);
		#endif
		goto end;
	}

#endif

#if 0
	/* wake the task up only when needed */
	if (/* changes on the production side */
	    (ic->flags & (CF_READ_NULL|CF_READ_ERROR)) ||
	    !si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST) ||
	    (si->flags & SI_FL_ERR) ||
	    ((ic->flags & CF_READ_PARTIAL) &&
	     ((ic->flags & CF_EOI) || !ic->to_forward || sio->state != SI_ST_EST)) ||

	    /* changes on the consumption side */
	    (oc->flags & (CF_WRITE_NULL|CF_WRITE_ERROR)) ||
	    ((oc->flags & CF_WRITE_ACTIVITY) &&
	     ((oc->flags & CF_SHUTW) ||
	      (((oc->flags & CF_WAKE_WRITE) ||
		!(oc->flags & (CF_AUTO_CLOSE|CF_SHUTW_NOW|CF_SHUTW))) &&
	       (sio->state != SI_ST_EST ||
	        (channel_is_empty(oc) && !oc->to_forward)))))) {
#else

	if_value_1 = (ic->flags & (CF_READ_NULL|CF_READ_ERROR));
	if_value_2 = !si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST);
	if_value_3 = (si->flags & SI_FL_ERR);
	if_value_4 = (ic->flags & CF_READ_PARTIAL) ;
	if_value_5 = (ic->flags & CF_EOI);
	if_value_6 = (if_value_5 || !ic->to_forward || sio->state != SI_ST_EST);
	if_value_7 = (oc->flags & (CF_WRITE_NULL|CF_WRITE_ERROR));
	if_value_8 = (oc->flags & CF_WRITE_ACTIVITY);

	if_value_9 = (oc->flags & CF_SHUTW);
	if_value_10 = (oc->flags & CF_WAKE_WRITE);
	if_value_11 = !(oc->flags & (CF_AUTO_CLOSE|CF_SHUTW_NOW|CF_SHUTW));
	if_value_12 = sio->state != SI_ST_EST;
	if_value_13 = (channel_is_empty(oc) && !oc->to_forward);

	if (/* changes on the production side */
	    if_value_1 || if_value_2 ||
	    if_value_3 || (if_value_4 && if_value_6) ||
	    /* changes on the consumption side */
	    if_value_7 || ( if_value_8 &&
	     (if_value_9|| (( if_value_10 || if_value_11) &&
	       ( if_value_12|| if_value_13 ))))) {
#endif
		SIPRINTF("[%s] task_wakeup\n", __func__);		

		task_wakeup(task, TASK_WOKEN_IO);
	}
	else {
		/* Update expiration date for the task and requeue it */
		task->expire = tick_first((tick_is_expired(task->expire, now_ms) ? 0 : task->expire),
					  tick_first(tick_first(ic->rex, ic->wex),
						     tick_first(oc->rex, oc->wex)));

		SIPRINTF("[%s] task_queue\n", __func__);

		task_queue(task);
	}

end:	
	if (ic->flags & CF_READ_ACTIVITY)
		ic->flags &= ~CF_READ_DONTWAIT; /* default run */
}


/* Called by I/O handlers after completion.. It propagates
 * connection flags to the stream interface, updates the stream (which may or
 * may not take this opportunity to try to forward data), then update the
 * connection's polling based on the channels and stream interface's final
 * states. The function always returns 0.
 */
static int si_cs_process(struct conn_stream *cs)
{
	struct connection *conn = cs->conn;
	struct stream_interface *si = cs->data;
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	/* If we have data to send, try it now */
	if (!channel_is_empty(oc) && !(si->wait_event.events & SUB_RETRY_SEND)) {
#if ENABLE_TIME_MEASURE		
		gettimeofday(&time_sicsp_send, NULL);
		si_cs_send(cs);
		gettimeofday(&time_sicsp_send_end, NULL);
#else
		SIPRINTF("[%s] si_cs_send at %d\n", __func__, __LINE__);
		si_cs_send(cs); 
#endif
	}
	/* First step, report to the stream-int what was detected at the
	 * connection layer : errors and connection establishment.
	 * Only add SI_FL_ERR if we're connected, or we're attempting to
	 * connect, we may get there because we got woken up, but only run
	 * after process_stream() noticed there were an error, and decided
	 * to retry to connect, the connection may still have CO_FL_ERROR,
	 * and we don't want to add SI_FL_ERR back
	 */
	if (si->state >= SI_ST_CON &&
	    (conn->flags & CO_FL_ERROR || cs->flags & CS_FL_ERROR)) {
			SIPRINTF("[%s] si->flags |= SI_FL_ERR\n", __func__);
		si->flags |= SI_FL_ERR;
	}

	/* If we had early data, and the handshake ended, then
	 * we can remove the flag, and attempt to wake the task up,
	 * in the event there's an analyser waiting for the end of
	 * the handshake.
	 */
	if (!(conn->flags & (CO_FL_HANDSHAKE | CO_FL_EARLY_SSL_HS)) &&
	    (cs->flags & CS_FL_WAIT_FOR_HS)) {
		SIPRINTF("[%s] task_wakeup(si_task(si), TASK_WOKEN_MSG)\n", __func__);
		cs->flags &= ~CS_FL_WAIT_FOR_HS;
		task_wakeup(si_task(si), TASK_WOKEN_MSG);
	}

	if (!si_state_in(si->state, SI_SB_EST|SI_SB_DIS|SI_SB_CLO) &&
	    (conn->flags & (CO_FL_CONNECTED | CO_FL_HANDSHAKE)) == CO_FL_CONNECTED) {
		SIPRINTF("[%s] si->exp = TICK_ETERNITY && oc->flags |= CF_WRITE_NULL\n", __func__);	
		si->exp = TICK_ETERNITY;
		oc->flags |= CF_WRITE_NULL;
		if (si->state == SI_ST_CON)
			si->state = SI_ST_RDY;
	}

	/* Report EOI on the channel if it was reached from the mux point of
	 * view. */
	if ((cs->flags & CS_FL_EOI) && !(ic->flags & CF_EOI)) {
		SIPRINTF("[%s] CF_EOI\n", __func__);	
		ic->flags |= (CF_EOI|CF_READ_PARTIAL);
	}

	/* Second step : update the stream-int and channels, try to forward any
	 * pending data, then possibly wake the stream up based on the new
	 * stream-int status.
	 */
#if ENABLE_TIME_MEASURE		
	gettimeofday(&time_sicsp_int, NULL);
	stream_int_notify(si);
	gettimeofday(&time_sicsp_int_end, NULL);
#else
	SIPRINTF("Enter stream_int_notify\n");

	stream_int_notify(si);	
	
#endif	

	channel_release_buffer(ic, &(si_strm(si)->buffer_wait));

	return 0;
}

/*
 * This function is called to send buffer data to a stream socket.
 * It calls the mux layer's snd_buf function. It relies on the
 * caller to commit polling changes. The caller should check conn->flags
 * for errors.
 */
int si_cs_send(struct conn_stream *cs)
{
	struct connection *conn = cs->conn;
	struct stream_interface *si = cs->data;
	struct channel *oc = si_oc(si);
	int ret;
	int did_send = 0;

	SIPRINTF("[%s]\n", __func__);

	/* We're already waiting to be able to send, give up */
	if (si->wait_event.events & SUB_RETRY_SEND)
		return 0;

	if (conn->flags & CO_FL_ERROR || cs->flags & (CS_FL_ERROR|CS_FL_ERR_PENDING)) {
		/* We're probably there because the tasklet was woken up,
		 * but process_stream() ran before, detected there were an
		 * error and put the si back to SI_ST_TAR. There's still
		 * CO_FL_ERROR on the connection but we don't want to add
		 * SI_FL_ERR back, so give up
		 */
		if (si->state < SI_ST_CON)
			return 0;
		si->flags |= SI_FL_ERR;
		return 1;
	}

	/* we might have been called just after an asynchronous shutw */
	if (conn->flags & CO_FL_SOCK_WR_SH || oc->flags & CF_SHUTW)
		return 1;

	if (oc->pipe && conn->xprt->snd_pipe && conn->mux->snd_pipe) {
		ret = conn->mux->snd_pipe(cs, oc->pipe);
		//if ((ret > 0) || empty_pipe) {
		if (ret > 0)
			did_send = 1;

#if ENABLE_CUJU_FT
		if (!oc->pipe->data && !oc->pipe->pipe_nxt) {
			ft_clean_pipe(oc->pipe);				
			////put_pipe(oc->pipe);
			oc->pipe = NULL;
		}
#else
		if (!oc->pipe->data) {
			put_pipe(oc->pipe);
			oc->pipe = NULL;
		}
#endif		

		if (oc->pipe)
			goto end;
	}

	/* At this point, the pipe is empty, but we may still have data pending
	 * in the normal buffer.
	 */
	if (co_data(oc)) {
		/* when we're here, we already know that there is no spliced
		 * data left, and that there are sendable buffered data.
		 */

		/* check if we want to inform the kernel that we're interested in
		 * sending more data after this call. We want this if :
		 *  - we're about to close after this last send and want to merge
		 *    the ongoing FIN with the last segment.
		 *  - we know we can't send everything at once and must get back
		 *    here because of unaligned data
		 *  - there is still a finite amount of data to forward
		 * The test is arranged so that the most common case does only 2
		 * tests.
		 */
		unsigned int send_flag = 0;

		if ((!(oc->flags & (CF_NEVER_WAIT|CF_SEND_DONTWAIT)) &&
		     ((oc->to_forward && oc->to_forward != CHN_INFINITE_FORWARD) ||
		      (oc->flags & CF_EXPECT_MORE))) ||
		    ((oc->flags & CF_ISRESP) &&
		     ((oc->flags & (CF_AUTO_CLOSE|CF_SHUTW_NOW)) == (CF_AUTO_CLOSE|CF_SHUTW_NOW))))
			send_flag |= CO_SFL_MSG_MORE;

		if (oc->flags & CF_STREAMER)
			send_flag |= CO_SFL_STREAMER;

		if ((si->flags & SI_FL_L7_RETRY) && !b_data(&si->l7_buffer)) {
			struct stream *s = si_strm(si);
			/* If we want to be able to do L7 retries, copy
			 * the data we're about to send, so that we are able
			 * to resend them if needed
			 */
			/* Try to allocate a buffer if we had none.
			 * If it fails, the next test will just
			 * disable the l7 retries by setting
			 * l7_conn_retries to 0.
			 */
			if (!s->txn || (s->txn->req.msg_state != HTTP_MSG_DONE))
				si->flags &= ~SI_FL_L7_RETRY;
			else {
				if (b_is_null(&si->l7_buffer))
					b_alloc(&si->l7_buffer);
				if (b_is_null(&si->l7_buffer))
					si->flags &= ~SI_FL_L7_RETRY;
				else {
					memcpy(b_orig(&si->l7_buffer),
					       b_orig(&oc->buf),
					       b_size(&oc->buf));
					si->l7_buffer.head = co_data(oc);
					b_add(&si->l7_buffer, co_data(oc));
				}

			}
		}

		ret = cs->conn->mux->snd_buf(cs, &oc->buf, co_data(oc), send_flag);
		if (ret > 0) {
			did_send = 1;
			co_set_data(oc, co_data(oc) - ret);
			c_realign_if_empty(oc);

			if (!co_data(oc)) {
				/* Always clear both flags once everything has been sent, they're one-shot */
				oc->flags &= ~(CF_EXPECT_MORE | CF_SEND_DONTWAIT);
			}
			/* if some data remain in the buffer, it's only because the
			 * system buffers are full, we will try next time.
			 */
		}
	}

 end:
 
	if (did_send) {
		cs->out_data += ret;
		g_out_data = cs->out_data;
		//printf("[%s] CS:%p Out Data:%lu\n", __func__, cs, cs->out_data);		
		
		oc->flags |= CF_WRITE_PARTIAL | CF_WROTE_DATA;
		if (si->state == SI_ST_CON)
			si->state = SI_ST_RDY;

		si_rx_room_rdy(si_opposite(si));
	}

	if (conn->flags & CO_FL_ERROR || cs->flags & (CS_FL_ERROR|CS_FL_ERR_PENDING)) {
		si->flags |= SI_FL_ERR;
		return 1;
	}

	/* We couldn't send all of our data, let the mux know we'd like to send more */
#if 0 //ENABLE_CUJU_FT
	/* more data to transmit for failover */
	if(!fdtab[conn->handle.fd].enable_migration)
		if (!channel_is_empty(oc))
			conn->mux->subscribe(cs, SUB_RETRY_SEND, &si->wait_event);
#endif
	
	/* UPSTREAM */
	if (!channel_is_empty(oc))
		conn->mux->subscribe(cs, SUB_RETRY_SEND, &si->wait_event);


	return did_send;
}

/* This is the ->process() function for any stream-interface's wait_event task.
 * It's assigned during the stream-interface's initialization, for any type of
 * stream interface. Thus it is always safe to perform a tasklet_wakeup() on a
 * stream interface, as the presence of the CS is checked there.
 */
struct task *si_cs_io_cb(struct task *t, void *ctx, unsigned short state)
{
	struct stream_interface *si = ctx;
	struct conn_stream *cs = objt_cs(si->end);
	struct channel *ic = si_ic(si);
	struct stream_interface *sio = si_opposite(si);

	int ret = 0;

	SIPRINTF("[%s] FD:%d\n", __func__, cs->conn->handle.fd);

	if (!cs)
		return NULL;

	if (!(si->wait_event.events & SUB_RETRY_SEND) && !channel_is_empty(si_oc(si))) {
		SIPRINTF("%s enter si_cs_send\n", __func__);

#if ENABLE_TIME_MEASURE
		gettimeofday(&time_sicsio_send, NULL);
		ret = si_cs_send(cs);
		gettimeofday(&time_sicsio_send_end, NULL);
#else
		ret |= si_cs_send(cs);
#endif	
		SIPRINTF("%s exit si_cs_send\n", __func__);
	}

	if (!(si->wait_event.events & SUB_RETRY_RECV)) {
		SIPRINTF("[%s] enter si_cs_recv | SUB_RETRY_RECV pass\n", __func__);

#if ENABLE_TIME_MEASURE
		gettimeofday(&time_sicsio_recv, NULL);
		ret |= si_cs_recv(cs);
		gettimeofday(&time_sicsio_recv_end, NULL);
#else		
		ret |= si_cs_recv(cs);
#endif
	}	

#if ENABLE_CUJU_FT
	if (ret != 0) {
		if (!(cs->conn->cujuipc_idx == 1)) {
			SIPRINTF("%s enter si_cs_process\n", __func__);

			//printf("[%s] CS:%p Out Data:%lu\n", __func__, cs, cs->out_data);
			//printf("[%s]  In Data:%lu\n", __func__, ic->total);

#if ENABLE_TIME_MEASURE
			gettimeofday(&time_sicsp, NULL);

			si_cs_process(cs);

			gettimeofday(&time_sicsp_end, NULL);

			time_in_sicsp = tv_to_us(&time_sicsp_end) -  tv_to_us(&time_sicsp);
#else			
			si_cs_process(cs);
#endif	
		}
		else {
			SIPRINTF("%s enter cuju_process ret = %d\n", __func__, ret);
			cuju_process(cs);
		}
	}
#else		
	if (ret != 0)
		si_cs_process(cs);
#endif
	return (NULL);
}

/* This function is designed to be called from within the stream handler to
 * update the input channel's expiration timer and the stream interface's
 * Rx flags based on the channel's flags. It needs to be called only once
 * after the channel's flags have settled down, and before they are cleared,
 * though it doesn't harm to call it as often as desired (it just slightly
 * hurts performance). It must not be called from outside of the stream
 * handler, as what it does will be used to compute the stream task's
 * expiration.
 */
void si_update_rx(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	if (ic->flags & CF_SHUTR) {
		si_rx_shut_blk(si);
		return;
	}

	/* Read not closed, update FD status and timeout for reads */
	if (ic->flags & CF_DONT_READ)
		si_rx_chan_blk(si);
	else
		si_rx_chan_rdy(si);

	if (!channel_is_empty(ic)) {
		/* stop reading, imposed by channel's policy or contents */
		si_rx_room_blk(si);
	}
	else {
		/* (re)start reading and update timeout. Note: we don't recompute the timeout
		 * everytime we get here, otherwise it would risk never to expire. We only
		 * update it if is was not yet set. The stream socket handler will already
		 * have updated it if there has been a completed I/O.
		 */
		si_rx_room_rdy(si);
	}
	if (si->flags & SI_FL_RXBLK_ANY & ~SI_FL_RX_WAIT_EP)
		ic->rex = TICK_ETERNITY;
	else if (!(ic->flags & CF_READ_NOEXP) && !tick_isset(ic->rex))
		ic->rex = tick_add_ifset(now_ms, ic->rto);

	si_chk_rcv(si);
}

/* This function is designed to be called from within the stream handler to
 * update the output channel's expiration timer and the stream interface's
 * Tx flags based on the channel's flags. It needs to be called only once
 * after the channel's flags have settled down, and before they are cleared,
 * though it doesn't harm to call it as often as desired (it just slightly
 * hurts performance). It must not be called from outside of the stream
 * handler, as what it does will be used to compute the stream task's
 * expiration.
 */
void si_update_tx(struct stream_interface *si)
{
	struct channel *oc = si_oc(si);
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	if (oc->flags & CF_SHUTW)
		return;

	/* Write not closed, update FD status and timeout for writes */
	if (channel_is_empty(oc)) {
		/* stop writing */
		if (!(si->flags & SI_FL_WAIT_DATA)) {
			if ((oc->flags & CF_SHUTW_NOW) == 0)
				si->flags |= SI_FL_WAIT_DATA;
			oc->wex = TICK_ETERNITY;
		}
		return;
	}

	/* (re)start writing and update timeout. Note: we don't recompute the timeout
	 * everytime we get here, otherwise it would risk never to expire. We only
	 * update it if is was not yet set. The stream socket handler will already
	 * have updated it if there has been a completed I/O.
	 */
	si->flags &= ~SI_FL_WAIT_DATA;
	if (!tick_isset(oc->wex)) {
		oc->wex = tick_add_ifset(now_ms, oc->wto);
		if (tick_isset(ic->rex) && !(si->flags & SI_FL_INDEP_STR)) {
			/* Note: depending on the protocol, we don't know if we're waiting
			 * for incoming data or not. So in order to prevent the socket from
			 * expiring read timeouts during writes, we refresh the read timeout,
			 * except if it was already infinite or if we have explicitly setup
			 * independent streams.
			 */
			ic->rex = tick_add_ifset(now_ms, ic->rto);
		}
	}
}

/* perform a synchronous send() for the stream interface. The CF_WRITE_NULL and
 * CF_WRITE_PARTIAL flags are cleared prior to the attempt, and will possibly
 * be updated in case of success.
 */
void si_sync_send(struct stream_interface *si)
{
	struct channel *oc = si_oc(si);
	struct conn_stream *cs;

	SIPRINTF("[%s] oc->flags:%08x si->state:%08x\n", __func__, oc->flags, si->state);

	oc->flags &= ~(CF_WRITE_NULL|CF_WRITE_PARTIAL);

	if (oc->flags & CF_SHUTW)
		return;

	if (channel_is_empty(oc))
		return;


	if (!si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST)) {
		return;
	}

	cs = objt_cs(si->end);
	if (!cs)
		return;

	if (cs->flags & (CS_FL_ERROR|CS_FL_ERR_PENDING))
		return;

	if (cs->conn->flags & CO_FL_ERROR)
		return;

	SIPRINTF("[%s] si_cs_send at %d\n", __func__, __LINE__);
	
	si_cs_send(cs);

	SIPRINTF("[%s] End oc->flags:%08x\n", __func__, oc->flags);
}

/* Updates at once the channel flags, and timers of both stream interfaces of a
 * same stream, to complete the work after the analysers, then updates the data
 * layer below. This will ensure that any synchronous update performed at the
 * data layer will be reflected in the channel flags and/or stream-interface.
 * Note that this does not change the stream interface's current state, though
 * it updates the previous state to the current one.
 */
void si_update_both(struct stream_interface *si_f, struct stream_interface *si_b)
{
	struct channel *req = si_ic(si_f);
	struct channel *res = si_oc(si_f);

	SIPRINTF("[%s]\n", __func__);
	SIPRINTF("[%s] req->flags:%08x \n", __func__, req->flags);
	SIPRINTF("[%s] res->flags:%08x \n", __func__, res->flags);

	SIPRINTF("[%s] si_f->state:%08x \n", __func__, si_f->state);
	SIPRINTF("[%s] si_b->state:%08x \n", __func__, si_b->state);

	req->flags &= ~(CF_READ_NULL|CF_READ_PARTIAL|CF_READ_ATTACHED|CF_WRITE_NULL|CF_WRITE_PARTIAL);
	res->flags &= ~(CF_READ_NULL|CF_READ_PARTIAL|CF_READ_ATTACHED|CF_WRITE_NULL|CF_WRITE_PARTIAL);

	si_f->prev_state = si_f->state;
	si_b->prev_state = si_b->state;

	/* let's recompute both sides states */
	if (si_state_in(si_f->state, SI_SB_RDY|SI_SB_EST))
		si_update(si_f);

	if (si_state_in(si_b->state, SI_SB_RDY|SI_SB_EST))
		si_update(si_b);

	/* stream ints are processed outside of process_stream() and must be
	 * handled at the latest moment.
	 */
	if (obj_type(si_f->end) == OBJ_TYPE_APPCTX &&
	    ((si_rx_endp_ready(si_f) && !si_rx_blocked(si_f)) ||
	     (si_tx_endp_ready(si_f) && !si_tx_blocked(si_f))))
		appctx_wakeup(si_appctx(si_f));

	if (obj_type(si_b->end) == OBJ_TYPE_APPCTX &&
	    ((si_rx_endp_ready(si_b) && !si_rx_blocked(si_b)) ||
	     (si_tx_endp_ready(si_b) && !si_tx_blocked(si_b))))
		appctx_wakeup(si_appctx(si_b));
}

/*
 * This function performs a shutdown-read on a stream interface attached to
 * a connection in a connected or init state (it does nothing for other
 * states). It either shuts the read side or marks itself as closed. The buffer
 * flags are updated to reflect the new state. If the stream interface has
 * SI_FL_NOHALF, we also forward the close to the write side. If a control
 * layer is defined, then it is supposed to be a socket layer and file
 * descriptors are then shutdown or closed accordingly. The function
 * automatically disables polling if needed.
 */
static void stream_int_shutr_conn(struct stream_interface *si)
{
	struct conn_stream *cs = __objt_cs(si->end);
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	si_rx_shut_blk(si);
	if (ic->flags & CF_SHUTR)
		return;
	ic->flags |= CF_SHUTR;
	ic->rex = TICK_ETERNITY;

	if (!si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST))
		return;

	if (si->flags & SI_FL_KILL_CONN)
		cs->flags |= CS_FL_KILL_CONN;

	if (si_oc(si)->flags & CF_SHUTW) {
		cs_close(cs);
		si->state = SI_ST_DIS;
		si->exp = TICK_ETERNITY;
	}
	else if (si->flags & SI_FL_NOHALF) {
		/* we want to immediately forward this close to the write side */
		return stream_int_shutw_conn(si);
	}
}

/*
 * This function performs a shutdown-write on a stream interface attached to
 * a connection in a connected or init state (it does nothing for other
 * states). It either shuts the write side or marks itself as closed. The
 * buffer flags are updated to reflect the new state.  It does also close
 * everything if the SI was marked as being in error state. If there is a
 * data-layer shutdown, it is called.
 */
static void stream_int_shutw_conn(struct stream_interface *si)
{
	struct conn_stream *cs = __objt_cs(si->end);
	struct connection *conn = cs->conn;
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	oc->flags &= ~CF_SHUTW_NOW;
	if (oc->flags & CF_SHUTW)
		return;
	oc->flags |= CF_SHUTW;
	oc->wex = TICK_ETERNITY;
	si_done_get(si);

	if (tick_isset(si->hcto)) {
		ic->rto = si->hcto;
		ic->rex = tick_add(now_ms, ic->rto);
	}

	switch (si->state) {
	case SI_ST_RDY:
	case SI_ST_EST:
		/* we have to shut before closing, otherwise some short messages
		 * may never leave the system, especially when there are remaining
		 * unread data in the socket input buffer, or when nolinger is set.
		 * However, if SI_FL_NOLINGER is explicitly set, we know there is
		 * no risk so we close both sides immediately.
		 */
		if (si->flags & SI_FL_KILL_CONN)
			cs->flags |= CS_FL_KILL_CONN;

		if (si->flags & SI_FL_ERR) {
			/* quick close, the socket is alredy shut anyway */
		}
		else if (si->flags & SI_FL_NOLINGER) {
			/* unclean data-layer shutdown, typically an aborted request
			 * or a forwarded shutdown from a client to a server due to
			 * option abortonclose. No need for the TLS layer to try to
			 * emit a shutdown message.
			 */
			cs_shutw(cs, CS_SHW_SILENT);
		}
		else {
			/* clean data-layer shutdown. This only happens on the
			 * frontend side, or on the backend side when forwarding
			 * a client close in TCP mode or in HTTP TUNNEL mode
			 * while option abortonclose is set. We want the TLS
			 * layer to try to signal it to the peer before we close.
			 */
			cs_shutw(cs, CS_SHW_NORMAL);

			if (!(ic->flags & (CF_SHUTR|CF_DONT_READ))) {
				/* OK just a shutw, but we want the caller
				 * to disable polling on this FD if exists.
				 */
				conn_cond_update_polling(conn);
				return;
			}
		}

		/* fall through */
	case SI_ST_CON:
		/* we may have to close a pending connection, and mark the
		 * response buffer as shutr
		 */
		if (si->flags & SI_FL_KILL_CONN)
			cs->flags |= CS_FL_KILL_CONN;
		cs_close(cs);
		/* fall through */
	case SI_ST_CER:
	case SI_ST_QUE:
	case SI_ST_TAR:
		si->state = SI_ST_DIS;
		/* fall through */
	default:
		si->flags &= ~SI_FL_NOLINGER;
		si_rx_shut_blk(si);
		ic->flags |= CF_SHUTR;
		ic->rex = TICK_ETERNITY;
		si->exp = TICK_ETERNITY;
	}
}

/* This function is used for inter-stream-interface calls. It is called by the
 * consumer to inform the producer side that it may be interested in checking
 * for free space in the buffer. Note that it intentionally does not update
 * timeouts, so that we can still check them later at wake-up. This function is
 * dedicated to connection-based stream interfaces.
 */
static void stream_int_chk_rcv_conn(struct stream_interface *si)
{
	SIPRINTF("[%s]\n", __func__);

	/* (re)start reading */
	if (si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST)) {
		tasklet_wakeup(si->wait_event.tasklet);
	}
}


/* This function is used for inter-stream-interface calls. It is called by the
 * producer to inform the consumer side that it may be interested in checking
 * for data in the buffer. Note that it intentionally does not update timeouts,
 * so that we can still check them later at wake-up.
 */
static void stream_int_chk_snd_conn(struct stream_interface *si)
{
	struct channel *oc = si_oc(si);
	struct conn_stream *cs = __objt_cs(si->end);

	SIPRINTF("[%s]\n", __func__);

	if (unlikely(!si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST) ||
	    (oc->flags & CF_SHUTW)))
		return;

	if (unlikely(channel_is_empty(oc)))  /* called with nothing to send ! */
		return;

	if (!oc->pipe &&                          /* spliced data wants to be forwarded ASAP */
		!(si->flags & SI_FL_WAIT_DATA))       /* not waiting for data */
		return;
		
	if (!(si->wait_event.events & SUB_RETRY_SEND) && !channel_is_empty(si_oc(si))) {
		SIPRINTF("[%s] enter si_cs_send\n", __func__);
		si_cs_send(cs);  /* default run */
	}

	if (cs->flags & (CS_FL_ERROR|CS_FL_ERR_PENDING) || cs->conn->flags & CO_FL_ERROR) {
		/* Write error on the file descriptor */
		if (si->state >= SI_ST_CON)
			si->flags |= SI_FL_ERR;
		goto out_wakeup;
	}

	/* OK, so now we know that some data might have been sent, and that we may
	 * have to poll first. We have to do that too if the buffer is not empty.
	 */
	if (channel_is_empty(oc)) {
		/* the connection is established but we can't write. Either the
		 * buffer is empty, or we just refrain from sending because the
		 * ->o limit was reached. Maybe we just wrote the last
		 * chunk and need to close.
		 */
		SIPRINTF("[%s] Channel Empty ID:%lu\n", __func__, pthread_self());


		if (((oc->flags & (CF_SHUTW|CF_AUTO_CLOSE|CF_SHUTW_NOW)) ==
		     (CF_AUTO_CLOSE|CF_SHUTW_NOW)) &&
		    si_state_in(si->state, SI_SB_RDY|SI_SB_EST)) {

			SIPRINTF("[%s] Ready Shutdown ID:%lu\n", __func__, pthread_self());	
			
			si_shutw(si);
			goto out_wakeup;
		}

		if ((oc->flags & (CF_SHUTW|CF_SHUTW_NOW)) == 0)
			si->flags |= SI_FL_WAIT_DATA;
		oc->wex = TICK_ETERNITY;
	}
	else {
		/* Otherwise there are remaining data to be sent in the buffer,
		 * which means we have to poll before doing so.
		 */
		si->flags &= ~SI_FL_WAIT_DATA;
		if (!tick_isset(oc->wex))
			oc->wex = tick_add_ifset(now_ms, oc->wto);
	}

	if (likely(oc->flags & CF_WRITE_ACTIVITY)) {
		struct channel *ic = si_ic(si);

		/* update timeout if we have written something */
		if ((oc->flags & (CF_SHUTW|CF_WRITE_PARTIAL)) == CF_WRITE_PARTIAL &&
		    !channel_is_empty(oc))
			oc->wex = tick_add_ifset(now_ms, oc->wto);

		if (tick_isset(ic->rex) && !(si->flags & SI_FL_INDEP_STR)) {
			/* Note: to prevent the client from expiring read timeouts
			 * during writes, we refresh it. We only do this if the
			 * interface is not configured for "independent streams",
			 * because for some applications it's better not to do this,
			 * for instance when continuously exchanging small amounts
			 * of data which can full the socket buffers long before a
			 * write timeout is detected.
			 */
			ic->rex = tick_add_ifset(now_ms, ic->rto);
		}
	}

	/* in case of special condition (error, shutdown, end of write...), we
	 * have to notify the task.
	 */
	if (likely((oc->flags & (CF_WRITE_NULL|CF_WRITE_ERROR|CF_SHUTW)) ||
	          ((oc->flags & CF_WAKE_WRITE) &&
	           ((channel_is_empty(oc) && !oc->to_forward) ||
	            !si_state_in(si->state, SI_SB_EST))))) {
	out_wakeup:
		if (!(si->flags & SI_FL_DONT_WAKE))
			task_wakeup(si_task(si), TASK_WOKEN_IO);
	}
}

/*
 * This is the callback which is called by the connection layer to receive data
 * into the buffer from the connection. It iterates over the mux layer's
 * rcv_buf function.
 */
int si_cs_recv(struct conn_stream *cs)
{
	struct connection *conn = cs->conn;
	struct stream_interface *si = cs->data;
	struct channel *ic = si_ic(si);
	int ret, max, cur_read = 0;
	int read_poll = MAX_READ_POLL_LOOPS;
	int flags = 0;

	SIPRINTF("[%s]\n", __func__);

	/* If another call to si_cs_recv() failed, and we subscribed to
	 * recv events already, give up now.
	 */
	if (si->wait_event.events & SUB_RETRY_RECV)
		return 0;

	SIRPRINTF("%s: CF_SHUTR \n", __func__);


	/* maybe we were called immediately after an asynchronous shutr */
	if (ic->flags & CF_SHUTR)
		return 1;

	SIRPRINTF("%s: CS_FL_EOS \n", __func__);

	/* stop here if we reached the end of data */
	if (cs->flags & CS_FL_EOS)
		goto out_shutdown_r;

	
	SIRPRINTF("%s: Check CS_FL_RCV_MORE \n", __func__);

	/* stop immediately on errors. Note that we DON'T want to stop on
	 * POLL_ERR, as the poller might report a write error while there
	 * are still data available in the recv buffer. This typically
	 * happens when we send too large a request to a backend server
	 * which rejects it before reading it all.
	 */
	if (!(cs->flags & CS_FL_RCV_MORE)) {
		if (!conn_xprt_ready(conn))
			return 0;
		if (conn->flags & CO_FL_ERROR || cs->flags & CS_FL_ERROR)
			return 1; // We want to make sure si_cs_wake() is called, so that process_strema is woken up, on failure
	}

	/* prepare to detect if the mux needs more room */
	cs->flags &= ~CS_FL_WANT_ROOM;
	
	SIRPRINTF("%s: (CF_STREAMER | CF_STREAMER_FAST)\n", __func__);
	
	if ((ic->flags & (CF_STREAMER | CF_STREAMER_FAST)) && !co_data(ic) &&
	    global.tune.idle_timer &&
	    (unsigned short)(now_ms - ic->last_read) >= global.tune.idle_timer) {
		/* The buffer was empty and nothing was transferred for more
		 * than one second. This was caused by a pause and not by
		 * congestion. Reset any streaming mode to reduce latency.
		 */
		ic->xfer_small = 0;
		ic->xfer_large = 0;
		ic->flags &= ~(CF_STREAMER | CF_STREAMER_FAST);
	}
	
	SIRPRINTF("%s: Check RCV_PIPE\n", __func__);

	/* First, let's see if we may splice data across the channel without
	 * using a buffer.
	 */
	if (conn->xprt->rcv_pipe && conn->mux->rcv_pipe &&
	    (ic->pipe || ic->to_forward >= MIN_SPLICE_FORWARD) &&
	    ic->flags & CF_KERN_SPLICING) {
		
		SIRPRINTF("%s: Enter Check RCV_PIPE\n", __func__);	

		if (c_data(ic)) {
			/* We're embarrassed, there are already data pending in
			 * the buffer and we don't want to have them at two
			 * locations at a time. Let's indicate we need some
			 * place and ask the consumer to hurry.
			 */
			flags |= CO_RFL_BUF_FLUSH;

			SIRPRINTF("%s: abort_splice 1\n", __func__);	

			goto abort_splice;
		}

		SIRPRINTF("%s: ic->pipe == NULL\n", __func__);

		if (unlikely(ic->pipe == NULL)) {
			if (pipes_used >= global.maxpipes || !(ic->pipe = get_pipe())) {
				ic->flags &= ~CF_KERN_SPLICING;
				
				SIRPRINTF("%s: abort_splice 2\n", __func__);

				goto abort_splice;
			}
		}
	
		SIRPRINTF("%s: conn->mux->rcv_pipe\n", __func__);
		
		
		ret = conn->mux->rcv_pipe(cs, ic->pipe, ic->to_forward);
		if (ret < 0) {
			/* splice not supported on this end, let's disable it */
			ic->flags &= ~CF_KERN_SPLICING;

			SIRPRINTF("%s: abort_splice 3\n", __func__);

			goto abort_splice;
		}


		SIPRINTF("%s: rcv_pipe ret is %d\n", __func__, ret);


		if (ret > 0) {
			if (ic->to_forward != CHN_INFINITE_FORWARD)
				ic->to_forward -= ret;
			ic->total += ret;

			g_in_data = ic->total;

			cur_read += ret;
			ic->flags |= CF_READ_PARTIAL;
			
			if (si->state == SI_ST_CON)
				si->state = SI_ST_RDY;
		}
		
		SIRPRINTF("%s: CS_FL_EOS\n", __func__);

		if (cs->flags & CS_FL_EOS)
			goto out_shutdown_r;

		SIRPRINTF("%s: CO_FL_ERROR\n", __func__);


		if (conn->flags & CO_FL_ERROR || cs->flags & CS_FL_ERROR)
			return 1;

	
		SIRPRINTF("%s: conn->flags & CO_FL_WAIT_ROOM\n", __func__);

		if (conn->flags & CO_FL_WAIT_ROOM) {
			/* the pipe is full or we have read enough data that it
			 * could soon be full. Let's stop before needing to poll.
			 */
			si_rx_room_blk(si);

			SIRPRINTF("%s: done_recv\n", __func__);

			goto done_recv;
		}

		/* splice not possible (anymore), let's go on on standard copy */
	}
	else {
		SIRPRINTF("%s: Enter Check RCV_PIPE ELSE\n", __func__);	

		/* be sure not to block regular receive path below */
		conn->flags &= ~CO_FL_WAIT_ROOM;
	}
	
	SIRPRINTF("%s: End of Check RCV_PIPE\n", __func__);


 abort_splice:
 #if ENABLE_CUJU_FT

	SIRPRINTF("%s: abort_splice check pipe \n", __func__);

 	
	 if (ic->pipe &&  unlikely(!ic->pipe->data) && (!ic->pipe->pipe_nxt)) {
		put_pipe(ic->pipe);
		ic->pipe = NULL;
	}
 #else
	if (ic->pipe && unlikely(!ic->pipe->data)) {
		put_pipe(ic->pipe);
		ic->pipe = NULL;
	}
#endif

	SIRPRINTF("%s: si_alloc_ibuf \n", __func__);


	/* now we'll need a input buffer for the stream */
	if (!si_alloc_ibuf(si, &(si_strm(si)->buffer_wait))) {

		SIRPRINTF("%s: goto end_recv\n", __func__);

		goto end_recv;
	}
	
	SIRPRINTF("%s: Check Flags\n", __func__);

	/* Important note : if we're called with POLL_IN|POLL_HUP, it means the read polling
	 * was enabled, which implies that the recv buffer was not full. So we have a guarantee
	 * that if such an event is not handled above in splice, it will be handled here by
	 * recv().
	 */
	while ((cs->flags & CS_FL_RCV_MORE) ||
	    (!(conn->flags & (CO_FL_ERROR | CO_FL_WAIT_ROOM | CO_FL_HANDSHAKE)) &&
	       (!(cs->flags & (CS_FL_ERROR|CS_FL_EOS))) && !(ic->flags & CF_SHUTR))) {

 
		SIRPRINTF("%s: Enter Check Flags\n", __func__);


		/* <max> may be null. This is the mux responsibility to set
		 * CS_FL_RCV_MORE on the CS if more space is needed.
		 */  
		max = channel_recv_max(ic);

		ret = cs->conn->mux->rcv_buf(cs, &ic->buf, max, flags | (co_data(ic) ? CO_RFL_BUF_WET : 0));

		SIRPRINTF("%s: Leave rcv_buf\n", __func__);		  


		if (cs->flags & CS_FL_WANT_ROOM) {
			SIRPRINTF("%s: Enter CS_FL_WANT_ROOM\n", __func__);

			si_rx_room_blk(si);
		}	

		if (cs->flags & CS_FL_READ_PARTIAL) {
			SIRPRINTF("%s: Enter CS_FL_READ_PARTIAL\n", __func__);

			if (tick_isset(ic->rex))
				ic->rex = tick_add_ifset(now_ms, ic->rto);
			cs->flags &= ~CS_FL_READ_PARTIAL;
		}

		if (ret <= 0) {
			SIRPRINTF("%s: Enter ret <= 0\n", __func__);

			break;
		}

		if (si->flags & SI_FL_L7_RETRY) {
			struct htx *htx;
			struct htx_sl *sl;

			htx = htxbuf(&ic->buf);
			if (htx) {
				sl = http_get_stline(htx);
				if (sl && l7_status_match(si_strm(si)->be,
				    sl->info.res.status)) {
					/* If we got a status for which we would
					 * like to retry the request, empty
					 * the buffer and pretend there's an
					 * error on the channel.
					 */
					ic->flags |= CF_READ_ERROR;
					htx_reset(htx);
					return 1;
				}
			}
			si->flags &= ~SI_FL_L7_RETRY;
		}
		cur_read += ret;

		SIRPRINTF("%s: Enter to_forward\n", __func__);

		/* if we're allowed to directly forward data, we must update ->o */
		if (ic->to_forward && !(ic->flags & (CF_SHUTW|CF_SHUTW_NOW))) {
			unsigned long fwd = ret;
			if (ic->to_forward != CHN_INFINITE_FORWARD) {
				if (fwd > ic->to_forward)
					fwd = ic->to_forward;
				ic->to_forward -= fwd;
			}
			c_adv(ic, fwd);
		}

		ic->flags |= CF_READ_PARTIAL;
		ic->total += ret;
		if (si->state == SI_ST_CON)
			si->state = SI_ST_RDY;

		SIRPRINTF("%s: Enter CF_READ_DONTWAIT\n", __func__);


		if ((ic->flags & CF_READ_DONTWAIT) || --read_poll <= 0) {
			/* we're stopped by the channel's policy */
			si_rx_chan_blk(si);
			break;
		}

		/* if too many bytes were missing from last read, it means that
		 * it's pointless trying to read again because the system does
		 * not have them in buffers.
		 */
		if (ret < max) {
			SIRPRINTF("%s: Enter ret < max\n", __func__);

			/* if a streamer has read few data, it may be because we
			 * have exhausted system buffers. It's not worth trying
			 * again.
			 */
			if (ic->flags & CF_STREAMER) {
				/* we're stopped by the channel's policy */
				si_rx_chan_blk(si);
				break;
			}

			/* if we read a large block smaller than what we requested,
			 * it's almost certain we'll never get anything more.
			 */
			if (ret >= global.tune.recv_enough) {
				/* we're stopped by the channel's policy */
				si_rx_chan_blk(si);
				break;
			}
		}

		/* if we are waiting for more space, don't try to read more data
		 * right now.
		 */
		if (si_rx_blocked(si))
			break;
		if (cs->conn->cujuipc_idx && cur_read > 0)
			break;
	} /* while !flags */

	SIRPRINTF("%s: Enter done_recv\n", __func__);


done_recv:
 	
	SIRPRINTF("%s: Enter end_recv\n", __func__);


	if (cur_read) {
		if ((ic->flags & (CF_STREAMER | CF_STREAMER_FAST)) &&
		    (cur_read <= ic->buf.size / 2)) {
			ic->xfer_large = 0;
			ic->xfer_small++;
			if (ic->xfer_small >= 3) {
				/* we have read less than half of the buffer in
				 * one pass, and this happened at least 3 times.
				 * This is definitely not a streamer.
				 */
				ic->flags &= ~(CF_STREAMER | CF_STREAMER_FAST);
			}
			else if (ic->xfer_small >= 2) {
				/* if the buffer has been at least half full twice,
				 * we receive faster than we send, so at least it
				 * is not a "fast streamer".
				 */
				ic->flags &= ~CF_STREAMER_FAST;
			}
		}
		else if (!(ic->flags & CF_STREAMER_FAST) &&
			 (cur_read >= ic->buf.size - global.tune.maxrewrite)) {
			/* we read a full buffer at once */
			ic->xfer_small = 0;
			ic->xfer_large++;
			if (ic->xfer_large >= 3) {
				/* we call this buffer a fast streamer if it manages
				 * to be filled in one call 3 consecutive times.
				 */
				ic->flags |= (CF_STREAMER | CF_STREAMER_FAST);
			}
		}
		else {
			ic->xfer_small = 0;
			ic->xfer_large = 0;
		}
		ic->last_read = now_ms;
	}

end_recv:
	
	SIRPRINTF("%s: Enter end_recv\n", __func__);

	if (conn->flags & CO_FL_ERROR || cs->flags & CS_FL_ERROR)
		return 1;

	if (cs->flags & CS_FL_EOS)
		/* connection closed */
		goto out_shutdown_r;

	/* Subscribe to receive events if we're blocking on I/O */
	if (!si_rx_blocked(si)) {
		SIRPRINTF("conn->mux->subscribe SUB_RETRY_RECV\n");
		
		conn->mux->subscribe(cs, SUB_RETRY_RECV, &si->wait_event);
		si_rx_endp_done(si);
	} else {
		si_rx_endp_more(si); /* first data */
	}

	return (cur_read != 0) || si_rx_blocked(si) || (cs->flags & CS_FL_EOI);

 out_shutdown_r:
	if (conn->flags & CO_FL_CONNECTED) {
		/* we received a shutdown */
		ic->flags |= CF_READ_NULL;
		if (ic->flags & CF_AUTO_CLOSE)
			channel_shutw_now(ic);
		stream_int_read0(si);
	}
	return 1;
}

/*
 * This function propagates a null read received on a socket-based connection.
 * It updates the stream interface. If the stream interface has SI_FL_NOHALF,
 * the close is also forwarded to the write side as an abort.
 */
static void stream_int_read0(struct stream_interface *si)
{
	struct conn_stream *cs = __objt_cs(si->end);
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	si_rx_shut_blk(si);
	if (ic->flags & CF_SHUTR)
		return;
	ic->flags |= CF_SHUTR;
	ic->rex = TICK_ETERNITY;

	if (!si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST))
		return;

	if (oc->flags & CF_SHUTW)
		goto do_close;

	if (si->flags & SI_FL_NOHALF) {
		/* we want to immediately forward this close to the write side */
		/* force flag on ssl to keep stream in cache */
		cs_shutw(cs, CS_SHW_SILENT);
		goto do_close;
	}

	/* otherwise that's just a normal read shutdown */
	return;

 do_close:
	/* OK we completely close the socket here just as if we went through si_shut[rw]() */
	cs_close(cs);

	oc->flags &= ~CF_SHUTW_NOW;
	oc->flags |= CF_SHUTW;
	oc->wex = TICK_ETERNITY;

	si_done_get(si);

	/* Don't change the state to SI_ST_DIS yet if we're still
	 * in SI_ST_CON, otherwise it means sess_establish() hasn't
	 * been called yet, and so the analysers would not run. However
	 * it's fine to switch to SI_ST_RDY as we have really validated
	 * the connection.
	 */
	if (si->state == SI_ST_EST)
		si->state = SI_ST_DIS;
	else if (si->state == SI_ST_CON)
		si->state = SI_ST_RDY;
	si->exp = TICK_ETERNITY;
	return;
}

/* Callback to be used by applet handlers upon completion. It updates the stream
 * (which may or may not take this opportunity to try to forward data), then
 * may re-enable the applet's based on the channels and stream interface's final
 * states.
 */
void si_applet_wake_cb(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	/* If the applet wants to write and the channel is closed, it's a
	 * broken pipe and it must be reported.
	 */
	if (!(si->flags & SI_FL_RX_WAIT_EP) && (ic->flags & CF_SHUTR))
		si->flags |= SI_FL_ERR;

	/* automatically mark the applet having data available if it reported
	 * begin blocked by the channel.
	 */
	if (si_rx_blocked(si))
		si_rx_endp_more(si);

	/* update the stream-int, channels, and possibly wake the stream up */
	stream_int_notify(si);

	/* stream_int_notify may have passed through chk_snd and released some
	 * RXBLK flags. Process_stream will consider those flags to wake up the
	 * appctx but in the case the task is not in runqueue we may have to
	 * wakeup the appctx immediately.
	 */
	if ((si_rx_endp_ready(si) && !si_rx_blocked(si)) ||
	    (si_tx_endp_ready(si) && !si_tx_blocked(si)))
		appctx_wakeup(si_appctx(si));
}

/*
 * This function performs a shutdown-read on a stream interface attached to an
 * applet in a connected or init state (it does nothing for other states). It
 * either shuts the read side or marks itself as closed. The buffer flags are
 * updated to reflect the new state. If the stream interface has SI_FL_NOHALF,
 * we also forward the close to the write side. The owner task is woken up if
 * it exists.
 */
static void stream_int_shutr_applet(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	si_rx_shut_blk(si);
	if (ic->flags & CF_SHUTR)
		return;
	ic->flags |= CF_SHUTR;
	ic->rex = TICK_ETERNITY;

	/* Note: on shutr, we don't call the applet */

	if (!si_state_in(si->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST))
		return;

	if (si_oc(si)->flags & CF_SHUTW) {
		si_applet_release(si);
		si->state = SI_ST_DIS;
		si->exp = TICK_ETERNITY;
	}
	else if (si->flags & SI_FL_NOHALF) {
		/* we want to immediately forward this close to the write side */
		return stream_int_shutw_applet(si);
	}
}

/*
 * This function performs a shutdown-write on a stream interface attached to an
 * applet in a connected or init state (it does nothing for other states). It
 * either shuts the write side or marks itself as closed. The buffer flags are
 * updated to reflect the new state. It does also close everything if the SI
 * was marked as being in error state. The owner task is woken up if it exists.
 */
static void stream_int_shutw_applet(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	oc->flags &= ~CF_SHUTW_NOW;
	if (oc->flags & CF_SHUTW)
		return;
	oc->flags |= CF_SHUTW;
	oc->wex = TICK_ETERNITY;
	si_done_get(si);

	if (tick_isset(si->hcto)) {
		ic->rto = si->hcto;
		ic->rex = tick_add(now_ms, ic->rto);
	}

	/* on shutw we always wake the applet up */
	appctx_wakeup(si_appctx(si));

	switch (si->state) {
	case SI_ST_RDY:
	case SI_ST_EST:
		/* we have to shut before closing, otherwise some short messages
		 * may never leave the system, especially when there are remaining
		 * unread data in the socket input buffer, or when nolinger is set.
		 * However, if SI_FL_NOLINGER is explicitly set, we know there is
		 * no risk so we close both sides immediately.
		 */
		if (!(si->flags & (SI_FL_ERR | SI_FL_NOLINGER)) &&
		    !(ic->flags & (CF_SHUTR|CF_DONT_READ)))
			return;

		/* fall through */
	case SI_ST_CON:
	case SI_ST_CER:
	case SI_ST_QUE:
	case SI_ST_TAR:
		/* Note that none of these states may happen with applets */
		si_applet_release(si);
		si->state = SI_ST_DIS;
	default:
		si->flags &= ~SI_FL_NOLINGER;
		si_rx_shut_blk(si);
		ic->flags |= CF_SHUTR;
		ic->rex = TICK_ETERNITY;
		si->exp = TICK_ETERNITY;
	}
}

/* chk_rcv function for applets */
static void stream_int_chk_rcv_applet(struct stream_interface *si)
{
	struct channel *ic = si_ic(si);

	SIPRINTF("[%s]\n", __func__);

	DPRINTF(stderr, "%s: si=%p, si->state=%d ic->flags=%08x oc->flags=%08x\n",
		__FUNCTION__,
		si, si->state, ic->flags, si_oc(si)->flags);

	if (!ic->pipe) {
		/* (re)start reading */
		appctx_wakeup(si_appctx(si));
	}
}

/* chk_snd function for applets */
static void stream_int_chk_snd_applet(struct stream_interface *si)
{
	struct channel *oc = si_oc(si);

	SIPRINTF("[%s]\n", __func__);

	DPRINTF(stderr, "%s: si=%p, si->state=%d ic->flags=%08x oc->flags=%08x\n",
		__FUNCTION__,
		si, si->state, si_ic(si)->flags, oc->flags);

	if (unlikely(si->state != SI_ST_EST || (oc->flags & CF_SHUTW)))
		return;

	/* we only wake the applet up if it was waiting for some data */

	if (!(si->flags & SI_FL_WAIT_DATA))
		return;

	if (!tick_isset(oc->wex))
		oc->wex = tick_add_ifset(now_ms, oc->wto);

	if (!channel_is_empty(oc)) {
		/* (re)start sending */
		appctx_wakeup(si_appctx(si));
	}
}

void si_wait_snapshot() 
{
	while (at_snapshot_time) {

	}
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
