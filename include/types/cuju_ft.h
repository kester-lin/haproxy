#ifndef _TYPES_CUKU_FT_H
#define _TYPES_CUKU_FT_H

#include <types/cuju_ft_def.h>
#include <proto/pipe.h>
#include <linux/netlink.h>
#include <libs/soccr.h>
#include <types/tcp_repair.h>


extern u_int32_t fake_pipe_cnt;
extern int pb_event;
extern u_int32_t at_snapshot_time;
extern u_int32_t cont_tx_idx;
extern u_int32_t snapshot_tx_idx; 
extern u_int32_t debug_test_flag;

extern u_int32_t r_pipe_create;
extern u_int32_t r_pipe_cancel;

extern u_int32_t ori_pipe_create;
extern u_int32_t ori_pipe_cancel;

extern struct timeval time_pre_snapshot;
extern struct timeval time_end_press;
extern unsigned long empty_queue_time;


extern pthread_mutex_t show_conn_mutex;

#define IP_LENGTH 4
#define DEFAULT_NIC_CNT 3
#define CONNECTION_LENGTH 12
#define DEFAULT_CONN_CNT 3
//#define DEFAULT_IPC_ARRAY  (24+(IP_LENGTH*DEFAULT_NIC_CNT)+(CONNECTION_LENGTH*DEFAULT_CONN_CNT))

#define TOTAL_NIC   8
#define TOTAL_CONN   8
#define DEFAULT_IPC_ARRAY  (20+(IP_LENGTH*TOTAL_NIC)+(CONNECTION_LENGTH*TOTAL_CONN))


#if ENABLE_CUJU_FT
extern struct proto_ipc *ipt_target;

extern u_int16_t fd_list_migration;
extern u_int32_t fd_pipe_cnt;
extern u_int16_t empty_pipe;
extern u_int16_t empty_pbuffer;
extern u_int16_t last_error;
extern u_int32_t guest_ip_db;

extern u_int16_t ipc_fd;

extern int trace_cnt;
extern int flush_cnt;

#if USING_NETLINK
/* NETLINK */
extern int nl_sock_fd;
extern struct msghdr nl_msg;
extern struct netlink_ipc nl_ipc;
extern struct nlmsghdr *nlh;
#endif

#if ENABLE_TIME_MEASURE_EPOLL	
extern struct timeval time_tepoll;
extern struct timeval time_tepoll_end;
extern unsigned long tepoll_time;	
#endif
#if ENABLE_TIME_MEASURE	

extern struct timeval time_poll;

extern struct timeval time_release;
extern struct timeval time_release_end;
extern unsigned long release_time;	

extern struct timeval time_loop;
extern struct timeval time_loop_end;
extern unsigned long loop_time;	

extern struct timeval time_recv;
extern struct timeval time_recv_end;
extern unsigned long time_in_recv;	

extern struct timeval time_send;
extern struct timeval time_send_end;
extern unsigned long time_in_send;	

extern struct timeval time_sicsp;
extern struct timeval time_sicsp_end;
extern unsigned long time_in_sicsp;	

extern struct timeval time_sicsio_send;
extern struct timeval time_sicsio_send_end;
extern unsigned long time_in_sicsio_send;	

extern struct timeval time_sicsio_recv;
extern struct timeval time_sicsio_recv_end;
extern unsigned long time_in_sicsio_recv;	

extern struct timeval time_sicsp_send;
extern struct timeval time_sicsp_send_end;
extern unsigned long time_in_sicsp_send;	

extern struct timeval time_sicsp_int;
extern struct timeval time_sicsp_int_end;
extern unsigned long time_in_sicsp_int;

#endif

#if ENABLE_CUJU_IPC

/* ipc_mode */
#define IPC_PACKET_CNT 1
#define IPC_PACKET_SIZE 2
#define IPC_TIME 3
#define IPC_SIGNAL 4
#define IPC_CUJU 5


#define COPY_PIPE_COPY 0
#define COPY_PIPE_CLEAN 1

/* Netlink FT action */
#define NL_TARGET_ADD_IN  0xFFFF
#define NL_TARGET_DEL_IN  0xEEEE  
#define NL_TARGET_ADD_OUT 0xDDDD
#define NL_TARGET_DEL_OUT 0xCCCC  


#define CONN_IS_SERVER 1
#define CONN_IS_CLIENT 0

/* cuju_ft_mode */
enum CUJU_FT_MODE
{
    CUJU_FT_ERROR = -1,
    CUJU_FT_OFF,
    CUJU_FT_INIT, // 1
    CUJU_FT_TRANSACTION_PRE_RUN,
    CUJU_FT_TRANSACTION_ITER,
    CUJU_FT_TRANSACTION_ATOMIC,
    CUJU_FT_TRANSACTION_RECV, // 5
    CUJU_FT_TRANSACTION_HANDOVER,
    CUJU_FT_TRANSACTION_SPECULATIVE,
    CUJU_FT_TRANSACTION_FLUSH_OUTPUT,
    CUJU_FT_TRANSACTION_TRANSFER,
    CUJU_FT_TRANSACTION_SNAPSHOT, // 10
    CUJU_FT_TRANSACTION_RUN,
    CUJU_FT_TRANSACTION_PRE_SNAPSHOT,
};


struct flush_work {
    u_int32_t flush_id;
    struct flush_work* fw_next;  
};

/* IPC PROTO */
struct proto_ipc
{
    u_int32_t ipc_mode:8;
    u_int32_t cuju_ft_arp:2;
    u_int32_t cuju_ft_mode:6;
    u_int32_t gft_id:16;
    u_int32_t epoch_id;
    u_int32_t flush_id;
    u_int32_t packet_cnt:16;
    u_int32_t packet_size:16;
    u_int32_t time_interval;
    u_int32_t nic_count:16;
    u_int32_t conn_count:16;
    unsigned int nic[TOTAL_NIC];
    unsigned char conn[TOTAL_CONN][CONNECTION_LENGTH];   
};

struct guest_ip_list
{
    u_int32_t guest_ip;
    struct gctl_ipc gctl_ipc;
    struct list list;
};


struct ft_fd_list
{
    u_int16_t ft_fd;
    struct list list;
};

struct netlink_ipc 
{
    u_int32_t epoch_id;
    u_int32_t flush_id;
    u_int16_t cuju_ft_mode;
    u_int16_t nic_count;
    unsigned int nic[TOTAL_NIC];
    u_int32_t conn_ip;
    u_int16_t conn_port; 
};

#endif /* End of ENABLE_CUJU_IPC*/


struct snapshot_data 
{
    u_int32_t vm_ip;
    u_int32_t epoch_id;

    /*保證存取操作的原子性 互斥性*/
    pthread_mutex_t locker;
    /*是否可讀*/
    pthread_cond_t cond;
};
struct vmsk_list 
{
    u_int32_t socket_id;
    struct connection *conn;
    //struct libsoccr_sk soccr;
    struct sk_data_info sk_data;
    struct list skid_list;
};



struct vm_list 
{
    u_int32_t vm_ip; /* using main NIC address nic[0] */
    u_int32_t nic[TOTAL_NIC];

    u_int8_t fault_enable;
    u_int8_t failovered;

    u_int8_t flush_idx;

    u_int32_t socket_count;
    pthread_mutex_t socket_metux;

    u_int32_t ipc_socket;
    struct vmsk_list skid_head;
    struct list vm_list;

    struct snapshot_data ss_data;

    //struct flush_work* fw_head;
    //struct flush_work* fw_tail;
};

struct fo_list {
  unsigned int nic;
  int socket_id;
  struct list fo_list;
};

struct thread_data 
{  
    int th_idx;
    int th_sock;
    int netlink_sock;
    struct proto_ipc* ipt_base;
};

struct recov_pipe {
	/* output pipe flush */
	struct pipe* first_pipe;
	u_int32_t first_byte;
	struct pipe* last_pipe;
	u_int32_t last_byte;
	u_int32_t first_number;
	u_int8_t first_idx;
	u_int32_t last_number;
	u_int8_t last_idx;	

	u_int32_t tr_count;
	u_int32_t tr_length;


    u_int32_t remainder;
    u_int32_t offset;
    struct recov_pipe* rp_next;  
};



extern struct vm_list vm_head;

int restore_one_tcp_conn(int fd, struct sk_data_info *data, struct vmsk_list *target_sk);

unsigned long ft_get_flushcnt();
unsigned long ft_get_epochcnt();
int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean);
int cuju_process(struct conn_stream *cs);
int ft_release_pipe_by_flush(struct pipe *pipe, uint32_t flush_id, uint16_t* total_pipe_cnt ,uint16_t* pipe_cnt);
int ft_release_pipe_by_transfer(struct pipe *pipe, uint16_t* total_pipe_cnt , uint16_t* pipe_cnt);
int ft_close_pipe(struct pipe *pipe,  int* pipe_cnt);
void ft_clean_pipe(struct pipe *pipe);
char *arp_get_ip(const char *req_mac);
void show_ft_time (void);
struct guest_ip_list *find_guestip_ptr(u_int32_t guest_ip);
u_int8_t find_guestip_exist(u_int32_t guest_ip);
struct guest_ip_list* add_guestip(u_int32_t guest_ip);
u_int8_t del_guestip(u_int32_t guest_ip);
struct guest_ip_list* check_guestip(u_int32_t source, u_int32_t dest, uint8_t* dir);
u_int8_t add_ft_fd(u_int16_t ftfd);
u_int16_t get_ft_fd(void);
__u64 tv_to_us(const struct timeval* tv);
u_int16_t getshmid(u_int32_t source, u_int32_t dest, u_int8_t *dir);


struct vm_list* target_in_table(struct list *table, u_int32_t vm_ip, u_int32_t socket_id);
struct vm_list *add_vm_target(struct list *table, u_int32_t vm_ip, u_int32_t socket_id, struct connection *conn);
struct vm_list* vm_in_table(struct list *table, u_int32_t vm_ip);
int del_target(struct list *table, u_int32_t vm_ip, u_int32_t socket_id);
int show_target_rule(struct list *table);
int clean_target_list(struct list *table);
void release_sk(struct libsoccr_sk *sk);

void* ipc_handler(void);
void* ipc_snapshot_in(void* data);

void ipc_snapshot_lock();
void ipc_snapshot_unlock();
void ipc_snapshot_trylock();
void ipc_snapshot_tryunlock();

int dump_tcp_conn_state_conn_zerocpy(int fd, struct sk_data_info *sk_data,
			    				     struct connection *conn, struct vm_list* target_vm);

u_int8_t show_conn_in_pipe(struct connection *conn);
u_int8_t show_conn_in_pipe_end(struct connection *conn);
u_int8_t show_conn_in_pipe_run(struct connection *conn);
int add_vmlist_by_conn(struct connection* conn, int cli_srv);

struct recov_pipe* rp_create();
u_int8_t rp_delete(struct recov_pipe* ptr);

u_int8_t fw_insert(struct connection *conn, u_int32_t flush_id);

u_int8_t add_to_flush_pipe_tail(struct connection *conn);

u_int8_t release_recovery_pipe_by_flush(struct connection *conn,
										struct recov_pipe* start, struct recov_pipe* end, 
										struct pipe* head, struct pipe* tail,
										u_int32_t *no_next_idx);
                                        
void show_recovery_pipe_list(struct recov_pipe * head);
#endif

#endif
