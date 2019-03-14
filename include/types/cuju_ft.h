#ifndef _TYPES_CUKU_FT_H
#define _TYPES_CUKU_FT_H

#include <types/cuju_ft_def.h>
#include <proto/pipe.h>

#if ENABLE_CUJU_FT
extern int fd_list_migration;
extern int fd_pipe_cnt;
extern int empty_pipe;

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
    u_int32_t cuju_ft_mode : 8;
    u_int32_t gft_id : 16;
    u_int32_t ephch_id;
    u_int32_t packet_cnt : 16;
    u_int32_t packet_size : 16;
    u_int32_t time_interval;
    u_int32_t nic_count : 16;
    u_int32_t conn_count : 16;
    unsigned char *conn_info;
};


#endif /* End of ENABLE_CUJU_IPC*/

unsigned long ft_get_flushcnt();
int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean);
void cuju_fd_handler(int fd);
int cuju_process(struct conn_stream *cs);
int ft_release_pipe(struct pipe *pipe, u_int32_t epoch_id, int* pipe_cnt);
int ft_close_pipe(struct pipe *pipe,  int* pipe_cnt);
void ft_clean_pipe(struct pipe *pipe);
#endif

#endif