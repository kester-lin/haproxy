#ifndef	_SHMEM_H
#define	_SHMEM_H

//#include <type.h>

typedef	unsigned char u_int8_t;
typedef	unsigned short int u_int16_t;
typedef	unsigned int u_int32_t;

enum CUJU_FT_MODE {
    CUJU_FT_ERROR = -1,
    CUJU_FT_OFF,
    CUJU_FT_INIT,                    // 1
    CUJU_FT_TRANSACTION_PRE_RUN,
    CUJU_FT_TRANSACTION_ITER,
    CUJU_FT_TRANSACTION_ATOMIC,
    CUJU_FT_TRANSACTION_RECV,        // 5
    CUJU_FT_TRANSACTION_HANDOVER,
    CUJU_FT_TRANSACTION_SPECULATIVE,
    CUJU_FT_TRANSACTION_FLUSH_OUTPUT,
    CUJU_FT_TRANSACTION_TRANSFER,
    CUJU_FT_TRANSACTION_SNAPSHOT,    // 10
    CUJU_FT_TRANSACTION_RUN,
};
/* only following using 
CUJU_FT_TRANSACTION_RUN,
CUJU_FT_TRANSACTION_FLUSH_OUTPUT,
CUJU_FT_TRANSACTION_HANDOVER
*/


#define KEY_SHM_CUJU_IPC     0x0500
#define SUPPORT_VM_CNT       100

#define IPC_PORT 1200


#define SEND_SAME_IP 1
#define IP_LENGTH 4
#define DEFAULT_NIC_CNT 3
#define CONNECTION_LENGTH 12
#define DEFAULT_CONN_CNT 3
//#define DEFAULT_IPC_ARRAY  (24+(IP_LENGTH*DEFAULT_NIC_CNT)+(CONNECTION_LENGTH*DEFAULT_CONN_CNT))

#define TOTAL_NIC   8
#define TOTAL_CONN   8

#define DEFAULT_IPC_ARRAY  (20 + (IP_LENGTH*TOTAL_NIC) + (CONNECTION_LENGTH * TOTAL_CONN))

struct thread_data 
{  
    int th_idx;
    int th_sock;
    int netlink_sock;
    struct proto_ipc* ipt_base;
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

struct netlink_ipc 
{
    u_int32_t epoch_id;
    u_int32_t flush_id;
    u_int16_t cuju_ft_mode;
    u_int16_t nic_count;
    unsigned int nic[TOTAL_NIC];
    unsigned char conn[TOTAL_CONN][CONNECTION_LENGTH]; 
};
#endif /* _SHMEM_H */
