#ifndef _TYPES_CUKU_FT_DEF_H
#define _TYPES_CUKU_FT_DEF_H

#include <common/hathreads.h>


#define ENABLE_CUJU_FT 1

#if ENABLE_CUJU_FT
#define ENABLE_EPOLL_MIGRATION      1
#define ENABLE_CUJU_IPC             1
#define ENABLE_EXTEND_CHECK	        0
#endif

struct gctl_ipc
{
    /* INDEX */
    u_int32_t packet_cnt_idx : 4;
    u_int32_t packet_size_idx : 4;
    u_int32_t trans_time_idx : 4;
    u_int32_t signal_idx : 4;
    u_int32_t cuju_ipc_idx : 4;

    /* control variable */
    u_int32_t packet_cnt : 16;
    u_int32_t packet_size : 16;
    
    uint32_t ephch_id;
    uint32_t flush_id;
    
    struct timeval last_trans_time;
    struct timeval target_time;


    __decl_hathreads(HA_RWLOCK_T lock);
};

extern struct gctl_ipc gctl_ipc;

#endif
