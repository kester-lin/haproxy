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
#include <sys/sendfile.h>


#ifdef DEBUG_FULL
#include <assert.h>
#endif


#if ENABLE_CUJU_FT
#define FAKE_CUJU_ID 0

#if FAKE_CUJU_ID
u_int32_t guest_ip_db = 0xd47ea8c0;
#else 

//u_int32_t guest_ip_db = 0xd47ea8c0;

/*
struct guest_ip_list
{
    u_int32_t guest_ip;
    struct list list;
};
*/
//static struct list pools = LIST_HEAD_INIT(pools);
static struct guest_ip_list gip_list = {
	.list = LIST_HEAD_INIT(gip_list.list)
};
#endif

#define DEBUG_CUJU_IPC 0
#if DEBUG_CUJU_IPC
#define CJIRPRINTF(x...) printf(x)
#else
#define CJIRPRINTF(x...)
#endif


#define DEBUG_CUJU_IPC_ID 0
#if DEBUG_CUJU_IPC_ID
#define CJIDRPRINTF(x...) printf(x)
#else
#define CJIDRPRINTF(x...)
#endif

#define MAC_LENGTH 4
#define DEFAULT_NIC_CNT 3
#define CONNECTION_LENGTH 12
#define DEFAULT_CONN_CNT 3
#define DEFAULT_IPC_ARRAY  (24+(MAC_LENGTH*DEFAULT_NIC_CNT)+(CONNECTION_LENGTH*DEFAULT_CONN_CNT))

u_int16_t fd_list_migration = 0;

static struct ft_fd_list ftfd_list = {
	.list = LIST_HEAD_INIT(ftfd_list.list)
};

u_int16_t fd_pipe_cnt = 0;
u_int16_t empty_pipe = 0;
u_int16_t empty_pbuffer = 0;
u_int16_t last_error = 0;

struct gctl_ipc gctl_ipc;

int pb_event = 0;

#if ENABLE_TIME_MEASURE_EPOLL
struct timeval time_tepoll;
struct timeval time_tepoll_end;
unsigned long tepoll_time;	
#endif

#if ENABLE_TIME_MEASURE
struct timeval time_poll;
struct timeval time_recv;
struct timeval time_recv_end;
struct timeval time_send;
struct timeval time_send_end;

struct timeval time_release;
struct timeval time_release_end;
unsigned long release_time = 0;	

struct timeval time_loop;
struct timeval time_loop_end;
unsigned long loop_time = 0;	
#endif
	
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
	unsigned long epoch_count = gctl_ipc.epoch_id;

	return epoch_count;
}

#endif


#define MAX_SPLICE_AT_ONCE	(1<<30)
#if 0
REGPRM1 static inline unsigned long tv_to_us(struct timeval *tv)
{
	unsigned long ret;

	ret  = tv->tv_sec * 1000 * 1000 + tv->tv_usec;

	return ret;
}
#else
/** Convert to micro-seconds */
static inline __u64 tv_to_us(const struct timeval* tv) 
{
        __u64 us = tv->tv_usec;
        us += (__u64)tv->tv_sec * (__u64)1000000;
        return us;
}
#endif

int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean)
{
	int ret = 0;
	static unsigned long retry_cnt = 0;

#if 1
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
#endif

	while (1) {
		if (clean == COPY_PIPE_CLEAN) {
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

#if ENABLE_TIME_MEASURE	
	struct timeval time_close;
	struct timeval time_close_end;
	unsigned long close_time = 0;		
#endif

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
				(*pipe_cnt)-=2;	
				printf("release pipe 2 total:%d\n", (*pipe_cnt));

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

#if ENABLE_TIME_MEASURE
	trace_cnt = 0;
	flush_cnt = 0;
	
	struct timeval time_close;
	struct timeval time_close_end;
	unsigned long close_time = 0;	
#endif

	//printf("%s  %d\n", __func__, *total_pipe_cnt);

	if (pipe->pipe_nxt) {

#if ENABLE_TIME_MEASURE		
		gettimeofday(&time_loop, NULL);
#endif		
		/* search next is empty insert new incoming to the pipe buffer tail */
		while (1) {
			if (pipe_trace == NULL) {
				//printf("pipe_trace NULL\n");
				break;
			}

			if (pipe_trace->pipe_nxt == NULL) {
				//printf("pipe_trace->pipe_nxt NULL\n");
				break;
			}

			//printf("pipe_trace: %p\n", pipe_trace);
			
			pipe_prev = pipe_trace;
			pipe_trace = pipe_trace->pipe_nxt;

#if ENABLE_TIME_MEASURE			
			trace_cnt++;
#endif
			//printf("Current pipe_trace: %p\n", pipe_trace);

			/* TODO: consider overflow */
			if (pipe_trace->flush_id < flush_id) {
#if ENABLE_TIME_MEASURE				
				gettimeofday(&time_release, NULL);
#endif
				//printf("pipe_trace->transfered: %p %p\n", pipe_trace->pipe_dup, pipe_trace);

				pipe_prev->pipe_nxt = pipe_trace->pipe_nxt;
				
				if(pipe_trace->pipe_dup) {
					ft_clean_pipe(pipe_trace->pipe_dup);
					put_pipe(pipe_trace->pipe_dup);
				}
				
				ft_clean_pipe(pipe_trace);				
				put_pipe(pipe_trace);

#if ENABLE_TIME_MEASURE				
				flush_cnt++;
#endif				
				(*pipe_cnt)-=2;
				(*total_pipe_cnt)-=2;

				printf("release pipe 2 total:%d\n", (*total_pipe_cnt));

				pipe_trace = pipe_prev->pipe_nxt;

#if ENABLE_TIME_MEASURE
				gettimeofday(&time_release_end, NULL);
				release_time = tv_to_us(&time_release_end) - tv_to_us(&time_release);
#endif
				//printf("[Flush]release_time time:%lu\n", release_time);
			}
		}
#if ENABLE_TIME_MEASURE		
		gettimeofday(&time_loop_end, NULL);
#endif	
	}
#if ENABLE_TIME_MEASURE	
	loop_time = tv_to_us(&time_loop_end) - tv_to_us(&time_loop);
#endif
	//printf("Flush trace count:%d flush:%d  loop:%lu\n", trace_cnt, flush_cnt, loop_time);

	return ret;
}

int ft_release_pipe_by_transfer(struct pipe *pipe, uint16_t* total_pipe_cnt , uint16_t* pipe_cnt)
{
	int ret = 0;
	struct pipe *pipe_trace = pipe;
	struct pipe *pipe_prev = NULL;

	int trace_cnt = 0;
	int transfer_cnt = 0;

	//printf("%s  %d\n", __func__, *total_pipe_cnt);

	if (pipe->pipe_nxt)
	{
		/* search pipe_nxt is empty insert new incoming to the pipe buffer tail */
		while (1)
		{
			if (pipe_trace == NULL)
			{
				//printf("pipe_trace NULL\n");
				break;
			}

			if (pipe_trace->pipe_nxt == NULL) {
				//printf("pipe_trace->pipe_nxt NULL\n");
				break;
			}
			
			//printf("pipe_trace: %p\n", pipe_trace);

			pipe_prev = pipe_trace;	
			pipe_trace = pipe_trace->pipe_nxt;

			//printf("Current pipe_trace: %p\n", pipe_trace);

			trace_cnt++;

			/* TODO: consider overflow */
			if (pipe_trace->transfered)
			{
				pipe_prev->pipe_nxt = pipe_trace->pipe_nxt;
				
				//printf("pipe_trace->transfered: %p %p\n", pipe_trace->pipe_dup, pipe_trace);
				//printf("pipe_trace->transfered: %p\n", pipe_trace);

				//ft_clean_pipe(pipe_trace->pipe_dup);
				//put_pipe(pipe_trace->pipe_dup);
				
				ft_clean_pipe(pipe_trace);				
				put_pipe(pipe_trace);
				
				printf("release pipe 1 total:%d\n", (*total_pipe_cnt));
				
				transfer_cnt++;
				(*pipe_cnt)--;
				(*total_pipe_cnt)--;

				pipe_trace = pipe_prev->pipe_nxt;
			}
			else {
				printf("Transfered End\n");
				break;
			}
		}
	}
	//printf("===================================\n");
	//printf ("Flush trace count:%d ted:%d\n", trace_cnt, transfer_cnt);

	return ret;
}

void ft_clean_pipe(struct pipe *pipe)
{
	pipe->pipe_dup = NULL;
	pipe->flush_id = 0;
	pipe->flush_idx = 0;
	pipe->epoch_id = 0;
	pipe->epoch_idx = 0;	
	pipe->in_fd = 0;
	pipe->out_fd = 0;
	pipe->trans_suspend = 0;
	pipe->transfer_cnt = 0;
	pipe->transfered = 0;
	pipe->pipe_nxt = NULL;
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
	u_int32_t guest_ip_db = 0;
	struct guest_ip_list* guest_info = NULL;

	//printf("%s\n", __func__);

	/* area may be zero */
	*(((char *)ic->buf.area) + ic->buf.data) = '\0';

	if (!ic->buf.data) {
		return -1;
	}

	//printf("%s data:%zu\n", __func__, ic->buf.data);

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

	//if ((ipc_ptr->cuju_ft_mode == CUJU_FT_INIT) || (ipc_ptr->cuju_ft_arp)) {
	if (ipc_ptr->cuju_ft_mode == CUJU_FT_INIT) {
		//printf ("VM NIC CNT: %d\n", ipc_ptr->nic_count);

		if (ipc_ptr->nic_count) {
			for(int idx = 0; idx < ipc_ptr->nic_count; idx++) {
				guest_ip_db = ntohl(*(((u_int32_t*)dynamic_array) + idx));

				printf("Find IP String %08x\n", guest_ip_db);
				
				if (ipc_ptr->cuju_ft_mode == CUJU_FT_INIT) {
					if (guest_info) {	
						/* add 2nd ip to original */
					}
					else {
						guest_info = add_guestip(guest_ip_db);
						if (!guest_info)
							assert(1);
					}
				}
			}
		}
	}
	
	if (ipc_ptr->cuju_ft_mode != CUJU_FT_INIT) {
		guest_ip_db = ntohl(*((u_int32_t*)dynamic_array));
		guest_info = find_guestip_ptr(guest_ip_db);
		if (!guest_info)
			assert(1);
	}
	
	/* Set GCTL */
	if (ipc_ptr->cuju_ft_mode == CUJU_FT_TRANSACTION_RUN) {
		guest_info->gctl_ipc.epoch_id = ipc_ptr->epoch_id;
		CJIDRPRINTF("Epoch ID:%u\n", ipc_ptr->epoch_id);
	}

	if (ipc_ptr->cuju_ft_mode == CUJU_FT_TRANSACTION_FLUSH_OUTPUT) {
		guest_info->gctl_ipc.flush_id =	ipc_ptr->epoch_id;
		CJIDRPRINTF("Flush ID:%u\n", ipc_ptr->epoch_id);
	}

	//printf("cuju_ft_mode is %d\n", ipc_ptr->cuju_ft_mode);

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


REGPRM1 static inline unsigned long tv_to_us_show(struct timeval *tv)
{
	printf("S: %lx\n", tv->tv_sec);
	printf("U: %lx\n", tv->tv_usec);
	return 0;
}

unsigned long max_send_time = 0;
unsigned long max_recv_time = 0;

void show_ft_time(void)
{
#if ENABLE_TIME_MEASURE	
	unsigned long send_time = 0;
	unsigned long recv_time = 0;
#endif
#if 0
	printf("\n");
	printf("%lx   %lx    %lx    %lx    %lx\n", 
		   tv_to_us(&time_poll), tv_to_us(&time_recv), tv_to_us(&time_recv_end),
		   tv_to_us(&time_send), tv_to_us(&time_send_end));
#endif
#if 1

#if 0
	printf("*******************************\n");
	tv_to_us_show(&time_poll);
	tv_to_us_show(&time_recv);
	tv_to_us_show(&time_recv_end);
	tv_to_us_show(&time_send);
	tv_to_us_show(&time_send_end);
	printf("***************END*************\n");
#endif
#if ENABLE_TIME_MEASURE
	send_time = tv_to_us(&time_send_end) - tv_to_us(&time_send);
	recv_time = tv_to_us(&time_recv_end)- tv_to_us(&time_recv);


	if (recv_time > max_recv_time) {
		max_recv_time = recv_time;
	}

	if (send_time > max_send_time) {
		max_send_time = send_time;
	}

	printf("%lu %lu %lu %lu\n\n", recv_time, send_time, max_recv_time, max_send_time);
#endif
#endif
#if 0
	if (pb_event) {
		printf("%lu %lu\n\n", 
			   tv_to_us(&time_send)- tv_to_us(&time_poll),
			   tv_to_us(&time_send_end) - tv_to_us(&time_send));
	}
	else {
		printf("%lu    %lu    %lu    %lu\n\n", 
			tv_to_us(&time_recv) - tv_to_us(&time_poll), 
			tv_to_us(&time_recv_end) - tv_to_us(&time_recv),
			tv_to_us(&time_send) - tv_to_us(&time_recv_end), 
			tv_to_us(&time_send_end) - tv_to_us(&time_send));
	}
#endif
}

struct guest_ip_list *find_guestip_ptr(u_int32_t guest_ip)
{

	struct guest_ip_list *gipl;

	list_for_each_entry(gipl, &gip_list.list, list) {
		if (gipl->guest_ip == guest_ip) {
			return gipl;
		}
	}
	return NULL;
}

u_int8_t find_guestip_exist(u_int32_t guest_ip)
{

	struct guest_ip_list *gipl;

	list_for_each_entry(gipl, &gip_list.list, list) {
		if (gipl->guest_ip == guest_ip) {
			return 1;
		}
	}
	return 0;
}

struct guest_ip_list* add_guestip(u_int32_t guest_ip)
{
	struct guest_ip_list *gipl;

	/* allocate list */
	gipl = calloc(1, sizeof(*gipl));
	if (!gipl) {
		printf("out of memory while indexing pattern\n");
		return NULL;
	}

	gipl->guest_ip = guest_ip;

	LIST_ADDQ(&gip_list.list, &gipl->list);

	return gipl;
}

u_int8_t del_guestip(u_int32_t guest_ip)
{
	struct guest_ip_list *gipl;

	list_for_each_entry(gipl, &gip_list.list, list) {
		if (gipl->guest_ip == guest_ip) {
			LIST_DEL(&gipl->list);
			free(gipl);
			return 1;
		}
	}
	return 0;
}

struct guest_ip_list* check_guestip(u_int32_t source, u_int32_t dest, uint8_t* dir)
{
	struct guest_ip_list *gipl;

	list_for_each_entry(gipl, &gip_list.list, list) {
		if (gipl->guest_ip == source) {
			*dir = DIR_DEST_CLIENT;
			return gipl;
		}

		if (gipl->guest_ip == dest) {
			*dir = DIR_DEST_GUEST;
			return gipl;
		}
	}

	printf("Miss guest IP\n");
	return NULL;
}

u_int16_t get_ft_fd(void)
{
	u_int16_t tmp_fd = 0;
	struct ft_fd_list* ftfdl;
	struct ft_fd_list* def ;

	list_for_each_entry_safe(ftfdl, def, &ftfd_list.list, list) {
		if (ftfdl->ft_fd != 0) {
			tmp_fd = ftfdl->ft_fd;
			/* release */
			ftfdl->ft_fd = 0;
			LIST_DEL(&ftfdl->list);
			free(ftfdl);
			return tmp_fd;
		}
	}

	return 0;
}

u_int8_t add_ft_fd(u_int16_t ftfd)
{
	struct ft_fd_list *ftfdl;

	/* allocate list */
	ftfdl = calloc(1, sizeof(*ftfdl));
	if (!ftfdl) {
		printf("out of memory while indexing pattern\n");
		return 0;
	}

	ftfdl->ft_fd = ftfd;

	LIST_ADDQ(&ftfd_list.list, &ftfdl->list);

	return 1;
}

#endif
