#ifndef _TYPES_CUKU_FT_H
#define _TYPES_CUKU_FT_H

#include <types/cuju_ft_def.h>
#include <proto/pipe.h>

#if ENABLE_CUJU_FT
extern u_int16_t fd_list_migration;
extern u_int16_t fd_pipe_cnt;
extern u_int16_t empty_pipe;
extern u_int32_t guest_ip_db;

#if ENABLE_CUJU_IPC

/* ipc_mode */
#define IPC_PACKET_CNT 1
#define IPC_PACKET_SIZE 2
#define IPC_TIME 3
#define IPC_SIGNAL 4
#define IPC_CUJU 5

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
};

struct proto_ipc
{
    u_int32_t transmit_cnt;
    u_int32_t ipc_mode : 8;
    u_int32_t cuju_ft_arp:2;
    u_int32_t cuju_ft_mode : 6;
    u_int32_t gft_id : 16;
    u_int32_t ephch_id;
    u_int32_t packet_cnt : 16;
    u_int32_t packet_size : 16;
    u_int32_t time_interval;
    u_int32_t nic_count : 16;
    u_int32_t conn_count : 16;
};


struct guest_ip
{
    u_int32_t guest_ip;
    struct list next;
};

#endif /* End of ENABLE_CUJU_IPC*/

unsigned long ft_get_flushcnt();
unsigned long ft_get_epochcnt();
int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean);
void cuju_fd_handler(int fd);
int cuju_process(struct conn_stream *cs);
int ft_release_pipe_by_flush(struct pipe *pipe, uint32_t flush_id, uint16_t* total_pipe_cnt ,uint16_t* pipe_cnt);
int ft_release_pipe_by_transfer(struct pipe *pipe, uint16_t* total_pipe_cnt , uint16_t* pipe_cnt);
int ft_close_pipe(struct pipe *pipe,  int* pipe_cnt);
void ft_clean_pipe(struct pipe *pipe);
char *arp_get_ip(const char *req_mac);
#endif

#endif