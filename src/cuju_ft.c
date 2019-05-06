#define _GNU_SOURCE

#include <errno.h>

#include <common/splice.h>
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
#define FAKE_CUJU_ID 1

#if FAKE_CUJU_ID
u_int32_t guest_ip_db = 0xd47ea8c0;
#else 
u_int32_t guest_ip_db = 0x0;
#endif

#define MAC_LENGTH 4
#define DEFAULT_NIC_CNT 3
#define CONNECTION_LENGTH 12
#define DEFAULT_CONN_CNT 3
#define DEFAULT_IPC_ARRAY  (24+(MAC_LENGTH*DEFAULT_NIC_CNT)+(CONNECTION_LENGTH*DEFAULT_CONN_CNT))

u_int16_t fd_list_migration = 0;
u_int16_t fd_pipe_cnt = 0;
u_int16_t empty_pipe = 0;

u_int32_t debug_var = 0;


struct gctl_ipc gctl_ipc;

#if FAKE_CUJU_ID
/* FAKE */
unsigned long flush_count = 1;
unsigned long epoch_count = 1;

unsigned long ft_get_flushcnt()
{
	return flush_count++;
}

unsigned long ft_get_epochcnt()
{
	return epoch_count++;
}
#else
/* FAKE */
//unsigned long flush_count = 0;
unsigned long ft_get_flushcnt()
{
	/* FAKE */
	//static unsigned long flush_count = 0;

	/* REAL */
	unsigned long flush_count = gctl_ipc.flush_id;

	return flush_count;
}

//unsigned long epoch_count = 0;
unsigned long ft_get_epochcnt()
{
	/* FAKE */
	//static unsigned long flush_count = 0;

	/* REAL */
	unsigned long epoch_count = gctl_ipc.ephch_id;

	return epoch_count;
}

#endif



int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean)
{
	int ret = 0;
	static unsigned long retry_cnt = 0;

	if (!source->data) {
#ifdef DEBUG_FULL
		printf("assert in %s source data is zero\n", __func__);
		assert(1);
#else
		return 0;
#endif
	}

	if (dest->data) {
#ifdef DEBUG_FULL
		printf("assert in %s dest data is not empty\n", __func__);
		assert(1);
#else
		return 0;
#endif
	}

	while (1) {
		if (clean) {
			ret = splice(source->cons, NULL, dest->prod, NULL,
						 source->data, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
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

	if (source->epoch_idx) {
		dest->epoch_id = source->epoch_id;

		if (clean) {
			source->epoch_id = 0;
			source->epoch_idx = 0;
		}
	}

	if (source->flush_idx) {
		dest->flush_id = source->flush_id;

		if (clean) {
			source->flush_id = 0;
			source->flush_idx = 0;
		}
	}	

	return ret;
}


int ft_close_pipe(struct pipe *pipe, int* pipe_cnt)
{
	int ret = 0;
	struct pipe *pipe_trace = pipe;
	struct pipe *pipe_prev = NULL;

	if (pipe->pipe_nxt)
	{
		/* search pipe_next is empty insert new incoming to the pipe buffer tail */
		while (1)
		{
			if (pipe_trace == NULL)
			{
				break;
			}

			if (pipe_trace->pipe_nxt == NULL) {
				break;
			}
			
			pipe_prev = pipe_trace;
			pipe_trace = pipe_trace->pipe_nxt;

			pipe_prev->pipe_nxt = pipe_trace->pipe_nxt;

			//if () {

				ft_clean_pipe(pipe_trace->pipe_dup);
				put_pipe(pipe_trace->pipe_dup);
					
				ft_clean_pipe(pipe_trace);				
				put_pipe(pipe_trace);
				(*pipe_cnt)--;	

			//}
			
			pipe_trace = pipe_prev->pipe_nxt;
		}
	}
	return ret;
}

int ft_release_pipe_by_flush(struct pipe *pipe, uint32_t flush_id, uint16_t* total_pipe_cnt ,uint16_t* pipe_cnt)
{
	int ret = 0;
	struct pipe *pipe_trace = pipe;
	struct pipe *pipe_prev = NULL;

	if (pipe->pipe_nxt)
	{
		/* search next is empty insert new incoming to the pipe buffer tail */
		while (1)
		{
			if (pipe_trace == NULL)
			{
				break;
			}

			if (pipe_trace->pipe_nxt == NULL) {
				break;
			}
			
			pipe_prev = pipe_trace;
			pipe_trace = pipe_trace->pipe_nxt;

			/* TODO: consider overflow */
			if (pipe_trace->flush_id < flush_id)
			{
				pipe_prev->pipe_nxt = pipe_trace->pipe_nxt;
				
				if(pipe_trace->pipe_dup) {
					ft_clean_pipe(pipe_trace->pipe_dup);
					put_pipe(pipe_trace->pipe_dup);
				}
				
				ft_clean_pipe(pipe_trace);				
				put_pipe(pipe_trace);
				
				(*pipe_cnt)--;
				(*total_pipe_cnt)--;
				pipe_trace = pipe_prev->pipe_nxt;
			}
		}
	}

	return ret;
}

int ft_release_pipe_by_transfer(struct pipe *pipe, uint16_t* total_pipe_cnt , uint16_t* pipe_cnt)
{
	int ret = 0;
	struct pipe *pipe_trace = pipe;
	struct pipe *pipe_prev = NULL;

	if (pipe->pipe_nxt)
	{
		/* search pipe_nxt is empty insert new incoming to the pipe buffer tail */
		while (1)
		{
			if (pipe_trace == NULL)
			{
				break;
			}

			if (pipe_trace->pipe_nxt == NULL) {
				break;
			}
			
			pipe_prev = pipe_trace;
			pipe_trace = pipe_trace->pipe_nxt;

			/* TODO: consider overflow */
			if (pipe_trace->transfered)
			{
				pipe_prev->pipe_nxt = pipe_trace->pipe_nxt;
				
				ft_clean_pipe(pipe_trace->pipe_dup);
				put_pipe(pipe_trace->pipe_dup);
				
				ft_clean_pipe(pipe_trace);				
				put_pipe(pipe_trace);
				
				(*pipe_cnt)--;
				(*total_pipe_cnt)--;
				pipe_trace = pipe_prev->pipe_nxt;
			}
		}
	}

	return ret;
}

void ft_clean_pipe(struct pipe *pipe)
{
	pipe->pipe_dup = NULL;
	pipe->flush_id = 0;
	pipe->flush_idx = 0;
	pipe->in_fd = 0;
	pipe->out_fd = 0;
	pipe->trans_suspend = 0;
	pipe->transfer_cnt = 0;
	pipe->transfered = 0;
	pipe->pipe_nxt = NULL;
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

	//printf("cuju_fd_handler fd is %d\n", fd);

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
	while (unlikely(conn->flags & (CO_FL_HANDSHAKE | CO_FL_ERROR)))
	{
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
		((conn->flags & (CO_FL_XPRT_WR_ENA | CO_FL_ERROR | CO_FL_HANDSHAKE)) == CO_FL_XPRT_WR_ENA)) {
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
		((conn->flags & (CO_FL_XPRT_RD_ENA | CO_FL_WAIT_ROOM | CO_FL_ERROR | CO_FL_HANDSHAKE)) == CO_FL_XPRT_RD_ENA)) {
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
						  ((flags & (CO_FL_CONNECTED | CO_FL_HANDSHAKE)) != CO_FL_CONNECTED &&
						   (conn->flags & (CO_FL_CONNECTED | CO_FL_HANDSHAKE)) == CO_FL_CONNECTED))) &&
		conn->mux->wake && conn->mux->wake(conn) < 0)
		return;

	/* commit polling changes */
	conn->flags &= ~CO_FL_WILL_UPDATE;
	conn_cond_update_polling(conn);

	return;
}

int cuju_process(struct conn_stream *cs)
{
	struct connection *conn = cs->conn;
	struct stream_interface *si = cs->data;
	struct channel *ic = si_ic(si);
	//struct channel *oc = si_oc(si);
	//unsigned int idx = 0;
	struct proto_ipc *ipc_ptr = NULL;
	u_int32_t dynamic_ipc = 0;
	u_int8_t* dynamic_array = NULL;
	char macaddr[16];

	/* area may be zero */
	*(((char *)ic->buf.area) + ic->buf.data) = '\0';

	//printf("IPC Data Size %lu\n", ic->buf.data);

	if (!ic->buf.data) {
		return -1;
	}

	//printf("IPC Structure Size %lu\n", sizeof(struct proto_ipc));
	debug_var = 0;
	ipc_ptr = (struct proto_ipc *)ic->buf.area;
	
	if((ipc_ptr->nic_count <= DEFAULT_NIC_CNT) && 
	   (ipc_ptr->conn_count <= DEFAULT_CONN_CNT)) {
		if (ic->buf.data != DEFAULT_IPC_ARRAY) {
			return -1;
		}

		dynamic_ipc = (MAC_LENGTH * ipc_ptr->nic_count) + (CONNECTION_LENGTH * ipc_ptr->conn_count);
		dynamic_array = (u_int8_t*)ic->buf.area + sizeof(struct proto_ipc);
	}
	else {
		if (ic->buf.data != sizeof(struct proto_ipc) + dynamic_ipc) {
			return -1;
		}
	}

#if 0
	printf("============================================================\n"); 
	for( idx = 0; idx < ic->buf.data; idx++ )
	{
		printf("%02x ", (unsigned int)*(ic->buf.area + idx) & 0xFF);

		if (idx % 16 == 15)
			printf("\n"); 
	}
	printf("============================================================\n");
	
	printf("ipc_ptr->ephch_id: %d\n", ipc_ptr->ephch_id);
	printf("ipc_ptr->transmit_cnt: %d\n", ipc_ptr->transmit_cnt);
	printf("ipc_ptr->ipc_mode: %d\n", ipc_ptr->ipc_mode);
	printf("ipc_ptr->cuju_ft_mode: %d\n", ipc_ptr->cuju_ft_mode);
	printf("ipc_ptr->gft_id: %d\n", ipc_ptr->gft_id);
	printf("ipc_ptr->packet_cnt: %d\n", ipc_ptr->packet_cnt);
	printf("ipc_ptr->packet_size: %d\n", ipc_ptr->packet_size);
	printf("ipc_ptr->time_interval: %d\n", ipc_ptr->time_interval);
	printf("ipc_ptr->nic_count: %d\n", ipc_ptr->nic_count);
	printf("ipc_ptr->conn_count: %d\n", ipc_ptr->conn_count);
#endif

	if ((ipc_ptr->cuju_ft_mode == CUJU_FT_INIT) || (ipc_ptr->cuju_ft_arp)) {
		
		printf ("VM NIC CNT: %d\n", ipc_ptr->nic_count);

		if (ipc_ptr->nic_count) {
			for(int idx = 0; idx < ipc_ptr->nic_count; idx++) {

				printf("IP: %02x:%02x:%02x:%02x\n", 
					   *((u_int8_t*)dynamic_array), *((u_int8_t*)dynamic_array + 1), 
					   *((u_int8_t*)dynamic_array + 2), *((u_int8_t*)dynamic_array + 3));


				sprintf(macaddr, "%02x.%02x.%02x.%02x",
					    *((u_int8_t*)dynamic_array), *((u_int8_t*)dynamic_array + 1), 
					    *((u_int8_t*)dynamic_array + 2), *((u_int8_t*)dynamic_array + 3));

				printf("Find IP %s\n", macaddr);

				guest_ip_db = ntohl(*(u_int32_t*)dynamic_array);

				printf("Find IP String %08x\n", guest_ip_db);	
#if 0 /* check arp in haproxy */		
				mac_ip = arp_get_ip(macaddr);
						
				if (mac_ip == NULL)
					printf("Find IP NULL\n");
				else 
					printf("Find IP %s\n", mac_ip);
#endif
			}
		}
	}
	
	
	/* Set GCTL */
	if (ipc_ptr->cuju_ft_mode == CUJU_FT_TRANSACTION_RUN) {
		gctl_ipc.ephch_id = ipc_ptr->ephch_id;
	}

	if (ipc_ptr->cuju_ft_mode == CUJU_FT_TRANSACTION_FLUSH_OUTPUT) {
		gctl_ipc.flush_id =	ipc_ptr->ephch_id;
	}

	/* clear */
	ic->buf.data = 0;

	if (cs->conn->cujuipc_idx) {
		cs->conn->flags &= ~CO_FL_CURR_RD_ENA;

		conn_update_xprt_polling(conn);

		fd_cant_recv(conn->handle.fd);
	}

	return 0;
}

char *arp_get_ip(const char *req_mac)
{
    FILE           *proc;
	 char ip[16];
	 char mac[18];
	 char * reply = NULL;
 
    if (!(proc = fopen("/proc/net/arp", "r"))) {
        return NULL;
    }
 
    /* Skip first line */
	 while (!feof(proc) && fgetc(proc) != '\n');
 
	 /* Find ip, copy mac in reply */
	 reply = NULL;
    while (!feof(proc) && (fscanf(proc, " %15[0-9.] %*s %*s %17[A-Fa-f0-9:] %*s %*s", ip, mac) == 2)) {
		  if (strcmp(mac, req_mac) == 0) {
				//reply = safe_strdup(ip);
				reply = strdup(ip);
				break;
		  }
    }
 
    fclose(proc);
 
    return reply;
}









#endif