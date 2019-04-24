/*
 * RAW transport layer over SOCK_STREAM sockets.
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/tcp.h>

#include <common/buffer.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>

#include <proto/connection.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/log.h>
#include <proto/pipe.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

#include <types/global.h>
#include <types/cuju_ft.h>
#if defined(CONFIG_HAP_LINUX_SPLICE)
#include <common/splice.h>

#define PIPE_ASSERT 1

#if PIPE_ASSERT
#include <assert.h>
#endif

/* A pipe contains 16 segments max, and it's common to see segments of 1448 bytes
 * because of timestamps. Use this as a hint for not looping on splice().
 */
#define SPLICE_FULL_HINT	16*1448

/* how many data we attempt to splice at once when the buffer is configured for
 * infinite forwarding */
#define MAX_SPLICE_AT_ONCE	(1<<30)

/* Versions of splice between 2.6.25 and 2.6.27.12 were bogus and would return EAGAIN
 * on incoming shutdowns. On these versions, we have to call recv() after such a return
 * in order to find whether splice is OK or not. Since 2.6.27.13 we don't need to do
 * this anymore, and we can avoid this logic by defining ASSUME_SPLICE_WORKS.
 */

/* Returns :
 *   -1 if splice() is not supported
 *   >= 0 to report the amount of spliced bytes.
 *   connection flags are updated (error, read0, wait_room, wait_data).
 *   The caller must have previously allocated the pipe.
 */
int raw_sock_to_pipe(struct connection *conn, void *xprt_ctx, struct pipe *pipe, unsigned int count)
{
#ifndef ASSUME_SPLICE_WORKS
	static THREAD_LOCAL int splice_detects_close;
#endif
	int ret;
	int retval = 0;
	struct in_addr ipv4_to;
	struct in_addr ipv4_from;

	if (!conn_ctrl_ready(conn))
		return 0;

	if (!fd_recv_ready(conn->handle.fd))
		return 0;

	conn_refresh_polling_flags(conn);
	errno = 0;

#if ENABLE_CUJU_FT
	pipe->in_fd = conn->handle.fd;
#endif

	/* Under Linux, if FD_POLL_HUP is set, we have reached the end.
	 * Since older splice() implementations were buggy and returned
	 * EAGAIN on end of read, let's bypass the call to splice() now.
	 */
	if (unlikely(!(fdtab[conn->handle.fd].ev & FD_POLL_IN))) {
		/* stop here if we reached the end of data */
		if ((fdtab[conn->handle.fd].ev & (FD_POLL_ERR|FD_POLL_HUP)) == FD_POLL_HUP)
			goto out_read0;

		/* report error on POLL_ERR before connection establishment */
		if ((fdtab[conn->handle.fd].ev & FD_POLL_ERR) && (conn->flags & CO_FL_WAIT_L4_CONN)) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			errno = 0; /* let the caller do a getsockopt() if it wants it */
			goto leave;
		}
	}
	
	ipv4_to = ((struct sockaddr_in *)&conn->addr.to)->sin_addr;
	ipv4_from = ((struct sockaddr_in *)&conn->addr.from)->sin_addr;

	if (ipv4_to.s_addr == guest_ip_db) {
		conn->direction = DIR_DEST_GUEST;
		//printf("Dest. is Guest, Conn is %p\n", conn);
	}
	else if (ipv4_from.s_addr == guest_ip_db) {
		conn->direction = DIR_DEST_CLIENT;
		//printf("Dest. is Client Application, Conn is %p\n", conn);
	} 
	else {
		//printf("Check Dest. Error\n");
	}


	while (count) {
		if (count > MAX_SPLICE_AT_ONCE)
			count = MAX_SPLICE_AT_ONCE;

		ret = splice(conn->handle.fd, NULL, pipe->prod, NULL, count,
					 SPLICE_F_MOVE|SPLICE_F_NONBLOCK);

		if (ret <= 0) {
			if (ret == 0) {
				/* connection closed. This is only detected by
				 * recent kernels (>= 2.6.27.13). If we notice
				 * it works, we store the info for later use.
				 */
#ifndef ASSUME_SPLICE_WORKS
				splice_detects_close = 1;
#endif
				goto out_read0;
			}

			if (errno == EAGAIN) {
				/* there are two reasons for EAGAIN :
				 *   - nothing in the socket buffer (standard)
				 *   - pipe is full
				 *   - the connection is closed (kernel < 2.6.27.13)
				 * The last case is annoying but know if we can detect it
				 * and if we can't then we rely on the call to recv() to
				 * get a valid verdict. The difference between the first
				 * two situations is problematic. Since we don't know if
				 * the pipe is full, we'll stop if the pipe is not empty.
				 * Anyway, we will almost always fill/empty the pipe.
				 */
				if (pipe->data) {
					/* alway stop reading until the pipe is flushed */
					conn->flags |= CO_FL_WAIT_ROOM;
					break;
				}

				/* We don't know if the connection was closed,
				 * but if we know splice detects close, then we
				 * know it for sure.
				 * But if we're called upon POLLIN with an empty
				 * pipe and get EAGAIN, it is suspect enough to
				 * try to fall back to the normal recv scheme
				 * which will be able to deal with the situation.
				 */
#ifndef ASSUME_SPLICE_WORKS
				if (splice_detects_close)
#endif
					fd_cant_recv(conn->handle.fd); /* we know for sure that it's EAGAIN */
				break;
			}
			else if (errno == ENOSYS || errno == EINVAL || errno == EBADF) {
				/* splice not supported on this end, disable it.
				 * We can safely return -1 since there is no
				 * chance that any data has been piped yet.
				 */
				retval = -1;
				goto leave;
			}
			else if (errno == EINTR) {
				/* try again */
				continue;
			}
			/* here we have another error */
			conn->flags |= CO_FL_ERROR;
			break;
		} /* ret <= 0 */

		/* Recv the Data tag epoch id*/
		if ((pipe->epoch_idx == 0) && (conn->direction == DIR_DEST_CLIENT)) {
			pipe->epoch_id = ft_get_epochcnt();
			pipe->epoch_idx = 1; 
		}

		retval += ret;
		pipe->data += ret;
		count -= ret;

		if (pipe->data >= SPLICE_FULL_HINT || ret >= global.tune.recv_enough) {
			/* We've read enough of it for this time, let's stop before
			 * being asked to poll.
			 */
			conn->flags |= CO_FL_WAIT_ROOM;
			fd_done_recv(conn->handle.fd);
			break;
		}
	} /* while */

	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN) && retval)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;

leave:
	conn_cond_update_sock_polling(conn);
	return retval;

out_read0:
	conn_sock_read0(conn);

	fd_list_migration = 0;
	fdtab[conn->handle.fd].enable_migration = 0;
	empty_pipe = 1;
	printf("CURRENT PIPE CNT IN RELEASE:%d\n", fd_pipe_cnt);
	printf("Close FD:%d\n", conn->handle.fd);

	conn->flags &= ~CO_FL_WAIT_L4_CONN;
	goto leave;
}

/* Send as many bytes as possible from the pipe to the connection's socket.
 */
int raw_sock_from_pipe(struct connection *conn, void *xprt_ctx, struct pipe *pipe)
{
	int ret = 0;
	int done = 0;

#if ENABLE_CUJU_FT
	struct pipe *pipe_trace = pipe;
	struct pipe *pipe_buf = NULL;
	struct pipe *pipe_dup = NULL;
	struct pipe *pipe_trans = NULL;
	int t_flag = 0;
	u_int32_t curr_flush_id;
	struct in_addr ipv4_to;
	struct in_addr ipv4_from;	
#endif

	if (!conn_ctrl_ready(conn))
		return 0;

	if (!fd_send_ready(conn->handle.fd))
		return 0;

	conn_refresh_polling_flags(conn);

#if ENABLE_CUJU_FT	
	
	pipe->out_fd = conn->handle.fd;

	if (pipe->data) {
		pipe_buf = get_pipe();
		pipe_dup = get_pipe();

		if (pipe_buf == NULL) {
#if PIPE_ASSERT
			assert(0);
#else
			return 0;
#endif			
		}
		if (pipe_dup == NULL) {
#if PIPE_ASSERT
			assert(0);
#else
			return 0;
#endif			
		}

		ft_dup_pipe(pipe, pipe_buf, 0);
		ft_dup_pipe(pipe, pipe_dup, 1);
		fd_pipe_cnt++;

		if(conn->direction == DIR_DEST_GUEST) {
			conn->backend_pipecnt++;
		}
		else if (conn->direction == DIR_DEST_CLIENT) {
			conn->frontend_pipecnt++;
		}

		printf("Create Pipe CNT:%d\n", fd_pipe_cnt);
		printf("Pipe: %p\n", pipe_buf);
		printf("Pipe: %p\n\n", pipe_dup);
	}
	else {
		if (pipe->pipe_nxt == NULL)	{
			printf("########################NULL RETRANSMIT########################\n");
			goto after_send;
			}
	}

	fdtab[conn->handle.fd].enable_migration = 1;

	if (pipe->pipe_nxt) {
		/* search pipe_nxt is empty insert new incoming to the pipe buffer tail */
		while (1) {
			if (pipe_trace->pipe_nxt == NULL) {
				if(pipe->data) {
					pipe_trace->pipe_nxt = pipe_buf;
					pipe_trace->pipe_nxt->pipe_dup = pipe_dup;
				}
				break;
			}
	
			pipe_trace = pipe_trace->pipe_nxt;

			if((pipe_trace->pipe_dup) && pipe_trace->pipe_dup->data &&
				!(pipe_trace->trans_suspend) && !(pipe_trace->data)) {
				ft_dup_pipe(pipe_trace->pipe_dup, pipe_trace, 0);
			}
		}
	}
	else {
		if (pipe_buf == NULL || pipe_dup == NULL) {
#if DEBUG_FULL
			assert(1);
#else
			return 0;
#endif
		}

		pipe->pipe_nxt = pipe_buf;
		pipe->pipe_nxt->pipe_dup = pipe_dup;
	}
	
	done = 0;
	pipe_trans = pipe->pipe_nxt;
	pipe->data = 0;

	if (pipe_trans == NULL) { 
		printf("pipe_trans is NULL\n");
		assert(1);
	}

	ipv4_to = ((struct sockaddr_in *)&conn->addr.to)->sin_addr;
	ipv4_from = ((struct sockaddr_in *)&conn->addr.from)->sin_addr;

	if (ipv4_to.s_addr == guest_ip_db) {
		conn->direction = DIR_DEST_GUEST;
		//printf("[WRITE] Dest. is Guest, Conn is %p\n", conn);
	}
	else if (ipv4_from.s_addr == guest_ip_db) {
		conn->direction = DIR_DEST_CLIENT;
		//printf("[WRITE] Dest. is Client Application, Conn is %p\n", conn);
	} 
	else {
		//printf("Check Dest. Error\n");
	}


	if (conn->direction == DIR_DEST_GUEST) {

		curr_flush_id = ft_get_flushcnt();

		if(pipe_trans->flush_idx) {
			/* check for release */
			ft_release_pipe_by_flush(pipe, curr_flush_id, &fd_pipe_cnt ,&conn->backend_pipecnt);
			pipe_trans = pipe->pipe_nxt;

			if (pipe_trans == NULL) {
				printf("pipe_trans is NULL\n");
				empty_pipe = 1;
				goto after_send;
			}
		}
		else {
			pipe_trans->flush_idx = 1;
			pipe_trans->flush_id = curr_flush_id;
		}

		/* WRITE */
		while (pipe_trans->data) {
			if (!pipe_trans->transfered) {
				ret = splice(pipe_trans->cons, NULL, conn->handle.fd, NULL,
							pipe_trans->data, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

				if (ret <= 0) {
					//pipe_trans->trans_suspend = 1;
					t_flag = (errno == EAGAIN);

					//printf("RET is %d  t_flag is %d\n", ret, t_flag);
					if (ret == 0 || t_flag) {
						//printf("ret == 0 || t_flag\n");
						pipe_trans->trans_suspend = 1;
						fd_cant_send(conn->handle.fd);
						break;
					}
					else if (errno == EINTR) {
						//printf("errno == EINTR\n");
						continue;
					}

					//printf("CO_FL_ERROR\n");
					/* here we have another error */
					conn->flags |= CO_FL_ERROR;
					break;
				}

				done += ret;
				pipe_trans->data -= ret;
				pipe_trans->transfer_cnt++;
				pipe_trans->trans_suspend = 0;
				pipe_trans->transfered = 1;
				//printf("Transfered\n");
			}

			if (pipe_trans->pipe_nxt != NULL) {
				pipe_trans = pipe_trans->pipe_nxt;
				//printf("Transfered then goto pipe_nxt\n");
			}
			else {
				break;
			}		
		}
	}
	else if (conn->direction == DIR_DEST_CLIENT) {
		curr_flush_id = ft_get_epochcnt();
//
		while (pipe_trans->data) {
			if (curr_flush_id > pipe_trans->flush_id) {
				ret = splice(pipe_trans->cons, NULL, conn->handle.fd, NULL,
							pipe_trans->data, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

				if (ret <= 0) {
					//pipe_trans->trans_suspend = 1;
					t_flag = (errno == EAGAIN);

					//printf("RET is %d  t_flag is %d\n", ret, t_flag);
					if (ret == 0 || t_flag) {
						//printf("ret == 0 || t_flag\n");
						pipe_trans->trans_suspend = 1;
						fd_cant_send(conn->handle.fd);
						break;
					}
					else if (errno == EINTR) {
						//printf("errno == EINTR\n");
						continue;
					}

					//printf("CO_FL_ERROR\n");
					/* here we have another error */
					conn->flags |= CO_FL_ERROR;
					break;
				}

				done += ret;
				pipe_trans->data -= ret;
				pipe_trans->transfer_cnt++;
				pipe_trans->trans_suspend = 0;
				pipe_trans->transfered = 1;
				//printf("Transfered\n");

				/* Pipe Release for DEST */
				
			}

			if (pipe_trans->pipe_nxt != NULL) {
				pipe_trans = pipe_trans->pipe_nxt;
				//printf("Transfered then goto pipe_nxt\n");
			}
			else {
				break;
			}		
		}
		if (done) {
			ft_release_pipe_by_transfer(pipe, &fd_pipe_cnt , &conn->frontend_pipecnt);
		}
//
	}
	else {
		done = 0;
		while (pipe->data) {
			ret = splice(pipe->cons, NULL, conn->handle.fd, NULL, pipe->data,
						SPLICE_F_MOVE|SPLICE_F_NONBLOCK);

			if (ret <= 0) {
				if (ret == 0 || errno == EAGAIN) {
					fd_cant_send(conn->handle.fd);
					break;
				}
				else if (errno == EINTR)
					continue;

				/* here we have another error */
				conn->flags |= CO_FL_ERROR;
				break;
			}

			done += ret;
			pipe->data -= ret;
		}	
	}
#else
	done = 0;
	while (pipe->data) {
		ret = splice(pipe->cons, NULL, conn->handle.fd, NULL, pipe->data,
					 SPLICE_F_MOVE|SPLICE_F_NONBLOCK);

		if (ret <= 0) {
			if (ret == 0 || errno == EAGAIN) {
				fd_cant_send(conn->handle.fd);
				break;
			}
			else if (errno == EINTR)
				continue;

			/* here we have another error */
			conn->flags |= CO_FL_ERROR;
			break;
		}

		done += ret;
		pipe->data -= ret;
	}
#endif

after_send:

	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN) && done)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;

	conn_cond_update_sock_polling(conn);

#if ENABLE_CUJU_FT
	if (pipe->pipe_nxt == NULL) {
		//fd_list_migration = 0;
		//fdtab[conn->handle.fd].enable_migration = 0;
		//empty_pipe = 1;
		printf("FD done:%d\n", conn->handle.fd);
		return done;
	}

	if (fdtab[conn->handle.fd].enable_migration) {
		fd_list_migration = pipe_trans->out_fd;
		conn->flags |= CO_FL_XPRT_WR_ENA;
	}

#endif

	return done;
}

#endif /* CONFIG_HAP_LINUX_SPLICE */

/* Receive up to <count> bytes from connection <conn>'s socket and store them
 * into buffer <buf>. Only one call to recv() is performed, unless the
 * buffer wraps, in which case a second call may be performed. The connection's
 * flags are updated with whatever special event is detected (error, read0,
 * empty). The caller is responsible for taking care of those events and
 * avoiding the call if inappropriate. The function does not call the
 * connection's polling update function, so the caller is responsible for this.
 * errno is cleared before starting so that the caller knows that if it spots an
 * error without errno, it's pending and can be retrieved via getsockopt(SO_ERROR).
 */
static size_t raw_sock_to_buf(struct connection *conn, void *xprt_ctx, struct buffer *buf, size_t count, int flags)
{
	ssize_t ret;
	size_t try, done = 0;

	if (!conn_ctrl_ready(conn))
		return 0;

	if (!fd_recv_ready(conn->handle.fd))
		return 0;

	conn_refresh_polling_flags(conn);
	errno = 0;

	if (unlikely(!(fdtab[conn->handle.fd].ev & FD_POLL_IN))) {
		/* stop here if we reached the end of data */
		if ((fdtab[conn->handle.fd].ev & (FD_POLL_ERR|FD_POLL_HUP)) == FD_POLL_HUP)
			goto read0;

		/* report error on POLL_ERR before connection establishment */
		if ((fdtab[conn->handle.fd].ev & FD_POLL_ERR) && (conn->flags & CO_FL_WAIT_L4_CONN)) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			goto leave;
		}
	}

	/* read the largest possible block. For this, we perform only one call
	 * to recv() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again. A new attempt is made on
	 * EINTR too.
	 */
	while (count > 0) {
		try = b_contig_space(buf);
		if (!try)
			break;

		if (try > count)
			try = count;

		ret = recv(conn->handle.fd, b_tail(buf), try, 0);

		if (ret > 0) {
			b_add(buf, ret);
			done += ret;
			if (ret < try) {
				/* unfortunately, on level-triggered events, POLL_HUP
				 * is generally delivered AFTER the system buffer is
				 * empty, unless the poller supports POLL_RDHUP. If
				 * we know this is the case, we don't try to read more
				 * as we know there's no more available. Similarly, if
				 * there's no problem with lingering we don't even try
				 * to read an unlikely close from the client since we'll
				 * close first anyway.
				 */
				if (fdtab[conn->handle.fd].ev & FD_POLL_HUP)
					goto read0;

				if ((!fdtab[conn->handle.fd].linger_risk) ||
					(cur_poller.flags & HAP_POLL_F_RDHUP)) {
					fd_done_recv(conn->handle.fd);
					break;
				}
			}
			count -= ret;
		}
		else if (ret == 0) {
			goto read0;
		}
		else if (errno == EAGAIN || errno == ENOTCONN) {
			fd_cant_recv(conn->handle.fd);
			break;
		}
		else if (errno != EINTR) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			break;
		}
	}

	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN) && done)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;

leave:
	conn_cond_update_sock_polling(conn);
	return done;

read0:
	conn_sock_read0(conn);
	conn->flags &= ~CO_FL_WAIT_L4_CONN;

	/* Now a final check for a possible asynchronous low-level error
	 * report. This can happen when a connection receives a reset
	 * after a shutdown, both POLL_HUP and POLL_ERR are queued, and
	 * we might have come from there by just checking POLL_HUP instead
	 * of recv()'s return value 0, so we have no way to tell there was
	 * an error without checking.
	 */
	if (unlikely(fdtab[conn->handle.fd].ev & FD_POLL_ERR))
		conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
	goto leave;
}

/* Send up to <count> pending bytes from buffer <buf> to connection <conn>'s
 * socket. <flags> may contain some CO_SFL_* flags to hint the system about
 * other pending data for example, but this flag is ignored at the moment.
 * Only one call to send() is performed, unless the buffer wraps, in which case
 * a second call may be performed. The connection's flags are updated with
 * whatever special event is detected (error, empty). The caller is responsible
 * for taking care of those events and avoiding the call if inappropriate. The
 * function does not call the connection's polling update function, so the caller
 * is responsible for this. It's up to the caller to update the buffer's contents
 * based on the return value.
 */
static size_t raw_sock_from_buf(struct connection *conn, void *xprt_ctx, const struct buffer *buf, size_t count, int flags)
{
	ssize_t ret;
	size_t try, done;
	int send_flag;

	if (!conn_ctrl_ready(conn))
		return 0;

	if (!fd_send_ready(conn->handle.fd))
		return 0;

	conn_refresh_polling_flags(conn);
	done = 0;
	/* send the largest possible block. For this we perform only one call
	 * to send() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again.
	 */
	while (count) {
		try = b_contig_data(buf, done);
		if (try > count)
			try = count;

		send_flag = MSG_DONTWAIT | MSG_NOSIGNAL;
		if (try < count || flags & CO_SFL_MSG_MORE)
			send_flag |= MSG_MORE;

		ret = send(conn->handle.fd, b_peek(buf, done), try, send_flag);

		if (ret > 0) {
			count -= ret;
			done += ret;

			/* A send succeeded, so we can consier ourself connected */
			conn->flags |= CO_FL_CONNECTED;
			/* if the system buffer is full, don't insist */
			if (ret < try)
				break;
		}
		else if (ret == 0 || errno == EAGAIN || errno == ENOTCONN) {
			/* nothing written, we need to poll for write first */
			fd_cant_send(conn->handle.fd);
			break;
		}
		else if (errno != EINTR) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			break;
		}
	}
	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN) && done)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;

	conn_cond_update_sock_polling(conn);
	return done;
}

static int raw_sock_subscribe(struct connection *conn, void *xprt_ctx, int event_type, void *param)
{
	return conn_subscribe(conn, xprt_ctx, event_type, param);
}

static int raw_sock_unsubscribe(struct connection *conn, void *xprt_ctx, int event_type, void *param)
{
	return conn_unsubscribe(conn, xprt_ctx, event_type, param);
}

/* transport-layer operations for RAW sockets */
static struct xprt_ops raw_sock = {
	.snd_buf  = raw_sock_from_buf,
	.rcv_buf  = raw_sock_to_buf,
	.subscribe = raw_sock_subscribe,
	.unsubscribe = raw_sock_unsubscribe,
#if defined(CONFIG_HAP_LINUX_SPLICE)
	.rcv_pipe = raw_sock_to_pipe,
	.snd_pipe = raw_sock_from_pipe,
#endif
	.shutr    = NULL,
	.shutw    = NULL,
	.close    = NULL,
	.name     = "RAW",
};

__attribute__((constructor)) 
static void __raw_sock_init(void)
{
	xprt_register(XPRT_RAW, &raw_sock);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
