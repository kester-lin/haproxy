#ifndef _TYPES_TCP_REPAIR_H
#define _TYPES_TCP_REPAIR_H
#include <errno.h>

#include <common/splice.h>
#include <types/cuju_ft.h>
#include <types/global.h>
#include <types/fd.h>

#include <sys/shm.h>
#include <fcntl.h> 
#include <common/libnet-structures.h>
#include <common/libnet-macros.h>
#include <common/libnet-headers.h>
#include <common/libnet-functions.h>

#define PB_ALEN_INET	1
#define PB_ALEN_INET6	4


#define USING_SOCCR_LOG 1


#define BUILD_BUG_ON(condition)	((void)sizeof(char[1 - 2*!!(condition)]))
#define memzero_p(p)		memset(p, 0, sizeof(*p))
#define memzero(p, size)	memset(p, 0, size)

enum {
	TCPF_ESTABLISHED = (1 << 1),
	TCPF_SYN_SENT    = (1 << 2),
	TCPF_SYN_RECV    = (1 << 3),
	TCPF_FIN_WAIT1   = (1 << 4),
	TCPF_FIN_WAIT2   = (1 << 5),
	TCPF_TIME_WAIT   = (1 << 6),
	TCPF_CLOSE       = (1 << 7),
	TCPF_CLOSE_WAIT  = (1 << 8),
	TCPF_LAST_ACK    = (1 << 9),
	TCPF_LISTEN      = (1 << 10),
	TCPF_CLOSING     = (1 << 11),
};


/*
 * The TCP transition diagram for half closed connections
 *
 * ------------
 * FIN_WAIT1	\ FIN
 *			---------
 *		/ ACK   CLOSE_WAIT
 * -----------
 * FIN_WAIT2
 *			----------
 *		/ FIN   LAST_ACK
 * -----------
 * TIME_WAIT	\ ACK
 *			----------
 *			CLOSED
 *
 * How to get the TCP_CLOSING state
 *
 * -----------		----------
 * FIN_WAIT1	\/ FIN	FIN_WAIT1
 * -----------		----------
 *  CLOSING		CLOSING
 *		\/ ACK
 * -----------		----------
 *  TIME_WAIT		TIME_WAIT
 */

/* Restore a fin packet in a send queue first */
#define SNDQ_FIRST_FIN	(TCPF_FIN_WAIT1 | TCPF_FIN_WAIT2 | TCPF_CLOSING)
/* Restore fin in a send queue after restoring fi in the receive queue. */
#define SNDQ_SECOND_FIN (TCPF_LAST_ACK | TCPF_CLOSE)
#define SNDQ_FIN_ACKED	(TCPF_FIN_WAIT2 | TCPF_CLOSE)

#define RCVQ_FIRST_FIN	(TCPF_CLOSE_WAIT | TCPF_LAST_ACK | TCPF_CLOSE)
#define RCVQ_SECOND_FIN (TCPF_CLOSING)
#define RCVQ_FIN_ACKED	(TCPF_CLOSE)


#ifndef TCPOPT_SACK_PERM
#define TCPOPT_SACK_PERM TCPOPT_SACK_PERMITTED
#endif

#define SOCR_DATA_MIN_SIZE	(17 * sizeof(__u32))


struct libsoccr_sk {
	int fd;
	unsigned flags;
	char *recv_queue;
	char *send_queue;
	union libsoccr_addr *src_addr;
	union libsoccr_addr *dst_addr;
};


struct sk_addr{
    uint32_t src_addr;
    uint32_t dst_addr;
    uint16_t src_port;
    uint16_t dst_port;
};

typedef struct sk_data_info
{
    struct sk_addr sk_addr;
    struct libsoccr_sk_data sk_data;
    char* send_queue;
    char* recv_queue;
}dt_info;

int restore_sockaddr(union libsoccr_addr *sa,
		int family, u32 pb_port, u32 *pb_addr, u32 ifindex);
int tcp_repair_on(int fd);
int tcp_repair_off(int fd);
int set_queue_seq(struct libsoccr_sk *sk, int queue, __u32 seq);
int libsoccr_restore_queue_HA(struct libsoccr_sk *sk, dt_info *data, unsigned data_size,
		int queue, char *buf);
int restore_fin_in_snd_queue(int sk, int acked);
int send_fin_HA(struct libsoccr_sk *sk, dt_info *data,
		unsigned data_size, uint8_t flags);
int send_queue(struct libsoccr_sk *sk, int queue, char *buf, __u32 len);
int __send_queue(struct libsoccr_sk *sk, int queue, char *buf, __u32 len);
int ipv6_addr_mapped(union libsoccr_addr *addr);
int libsoccr_set_sk_data_noq_conn(struct libsoccr_sk *sk,
		struct sk_data_info *data, unsigned int data_size);
int libsoccr_restore_conn(struct libsoccr_sk *sk,
						  struct sk_data_info* data, 
						  unsigned int data_size);



#endif /* _TYPES_CUKU_FT_H */