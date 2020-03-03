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

#include <sys/shm.h>
#include <fcntl.h> 
#include <libs/soccr.h>
#include <types/tcp_repair.h>

#if USING_SOCCR_LOG
unsigned int log_level = 0;
static void (*log)(unsigned int loglevel, const char *format, ...)
	__attribute__ ((__format__ (__printf__, 2, 3)));
extern unsigned int log_level;
#define loge(msg, ...) do { if (log && (log_level >= SOCCR_LOG_ERR)) log(SOCCR_LOG_ERR, "Error (%s:%d): " msg, __FILE__, __LINE__, ##__VA_ARGS__); } while (0)
#define logerr(msg, ...) loge(msg ": %s\n", ##__VA_ARGS__, strerror(errno))
#define logd(msg, ...) do { if (log && (log_level >= SOCCR_LOG_DBG)) log(SOCCR_LOG_DBG, "Debug: " msg, ##__VA_ARGS__); } while (0)
#else 
#define loge(msg, ...)
#define logerr(msg, ...)
#define logd(msg, ...)
#endif

int restore_sockaddr(union libsoccr_addr *sa,
		int family, u32 pb_port, u32 *pb_addr, u32 ifindex)
{
	BUILD_BUG_ON(sizeof(sa->v4.sin_addr.s_addr) > PB_ALEN_INET * sizeof(u32));
	BUILD_BUG_ON(sizeof(sa->v6.sin6_addr.s6_addr) > PB_ALEN_INET6 * sizeof(u32));

	memzero(sa, sizeof(*sa));

	if (family == AF_INET) {
		sa->v4.sin_family = AF_INET;
		sa->v4.sin_port = htons(pb_port);
		memcpy(&sa->v4.sin_addr.s_addr, pb_addr, sizeof(sa->v4.sin_addr.s_addr));
		return sizeof(sa->v4);
	}

	if (family == AF_INET6) {
		sa->v6.sin6_family = AF_INET6;
		sa->v6.sin6_port = htons(pb_port);
		memcpy(sa->v6.sin6_addr.s6_addr, pb_addr, sizeof(sa->v6.sin6_addr.s6_addr));

		/* Here although the struct member is called scope_id, the
		 * kernel really wants ifindex. See
		 * /net/ipv6/af_inet6.c:inet6_bind for details.
		 */
		sa->v6.sin6_scope_id = ifindex;
		return sizeof(sa->v6);
	}

	//BUG();
	return -1;
}


int tcp_repair_on(int fd)
{
	int ret, aux = 1;

	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR, &aux, sizeof(aux));
	if (ret < 0)
		printf("Can't turn TCP repair mode ON");

	return ret;
}

int tcp_repair_off(int fd)
{
	int aux = 0, ret;

	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR, &aux, sizeof(aux));
	if (ret < 0)
		printf("Failed to turn off repair mode on socket");

	return ret;
}


int set_queue_seq(struct libsoccr_sk *sk, int queue, __u32 seq)
{
	logd("\tSetting %d queue seq to %u\n", queue, seq);

	if (setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		logerr("Can't set repair queue");
		return -1;
	}

	if (setsockopt(sk->fd, SOL_TCP, TCP_QUEUE_SEQ, &seq, sizeof(seq)) < 0) {
		logerr("Can't set queue seq");
		return -1;
	}

	return 0;
}

int libsoccr_restore_queue_HA(struct libsoccr_sk *sk, dt_info *data, unsigned data_size,
		int queue, char *buf)
{
	if (!buf)
		return 0;

	if (!data || data_size < SOCR_DATA_MIN_SIZE)
		return -1;

	if (queue == TCP_RECV_QUEUE) {
		if (!data->sk_data.inq_len)
			return 0;
		return send_queue(sk, TCP_RECV_QUEUE, buf, data->sk_data.inq_len);
	}

	if (queue == TCP_SEND_QUEUE) {
		__u32 len, ulen;

		/*
		 * All data in a write buffer can be divided on two parts sent
		 * but not yet acknowledged data and unsent data.
		 * The TCP stack must know which data have been sent, because
		 * acknowledgment can be received for them. These data must be
		 * restored in repair mode.
		 */
		ulen = data->sk_data.unsq_len;
		len = data->sk_data.outq_len - ulen;
		if (len && send_queue(sk, TCP_SEND_QUEUE, buf, len))
			return -2;

		if (ulen) {
			/*
			 * The second part of data have never been sent to outside, so
			 * they can be restored without any tricks.
			 */
			tcp_repair_off(sk->fd);
			if (__send_queue(sk, TCP_SEND_QUEUE, buf + len, ulen))
				return -3;
			if (tcp_repair_on(sk->fd))
				return -4;
		}

		return 0;
	}

	return -5;
}


int restore_fin_in_snd_queue(int sk, int acked)
{
	int queue = TCP_SEND_QUEUE;
	int ret;

	/*
	 * If TCP_SEND_QUEUE is set, a fin packet will be
	 * restored as a sent packet.
	 */
	if (acked &&
	    setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		logerr("Can't set repair queue");
		return -1;
	}

	ret = shutdown(sk, SHUT_WR);
	if (ret < 0)
		logerr("Unable to shut down a socket");

	queue = TCP_NO_QUEUE;
	if (acked &&
	    setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		logerr("Can't set repair queue");
		return -1;
	}

	return ret;
}

int send_fin_HA(struct libsoccr_sk *sk, dt_info *data,
		unsigned data_size, uint8_t flags)
{
	uint32_t src_v4 = sk->src_addr->v4.sin_addr.s_addr;
	uint32_t dst_v4 = sk->dst_addr->v4.sin_addr.s_addr;
	int ret, exit_code = -1, family;
	char errbuf[LIBNET_ERRBUF_SIZE];
	int mark = SOCCR_MARK;
	int libnet_type;
	libnet_t *l;

	family = sk->dst_addr->sa.sa_family;

	if (family == AF_INET6 && ipv6_addr_mapped(sk->dst_addr)) {
		/* TCP over IPv4 */
		family = AF_INET;
		dst_v4 = sk->dst_addr->v6.sin6_addr.s6_addr32[3];
		src_v4 = sk->src_addr->v6.sin6_addr.s6_addr32[3];
	}

	if (family == AF_INET6)
		libnet_type = LIBNET_RAW6;
	else
		libnet_type = LIBNET_RAW4;

	l = libnet_init(
		libnet_type,		/* injection type */
		NULL,			/* network interface */
		errbuf);		/* errbuf */
	if (l == NULL) {
		loge("libnet_init failed (%s)\n", errbuf);
		return -1;
	}

	if (setsockopt(l->fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark))) {
		logerr("Can't set SO_MARK (%d) for socket\n", mark);
		goto err;
	}

	ret = libnet_build_tcp(
		ntohs(sk->dst_addr->v4.sin_port),		/* source port */
		ntohs(sk->src_addr->v4.sin_port),		/* destination port */
		data->sk_data.inq_seq,			/* sequence number */
		data->sk_data.outq_seq - data->sk_data.outq_len,	/* acknowledgement num */
		flags,				/* control flags */
		data->sk_data.rcv_wnd,			/* window size */
		0,				/* checksum */
		10,				/* urgent pointer */
		LIBNET_TCP_H + 20,		/* TCP packet size */
		NULL,				/* payload */
		0,				/* payload size */
		l,				/* libnet handle */
		0);				/* libnet id */
	if (ret == -1) {
		loge("Can't build TCP header: %s\n", libnet_geterror(l));
		goto err;
	}

	if (family == AF_INET6) {
		struct libnet_in6_addr src, dst;

		memcpy(&dst, &sk->dst_addr->v6.sin6_addr, sizeof(dst));
		memcpy(&src, &sk->src_addr->v6.sin6_addr, sizeof(src));

		ret = libnet_build_ipv6(
			0, 0,
			LIBNET_TCP_H,	/* length */
			IPPROTO_TCP,	/* protocol */
			64,		/* hop limit */
			dst,		/* source IP */
			src,		/* destination IP */
			NULL,		/* payload */
			0,		/* payload size */
			l,		/* libnet handle */
			0);		/* libnet id */
	} else if (family == AF_INET)
		ret = libnet_build_ipv4(
			LIBNET_IPV4_H + LIBNET_TCP_H + 20,	/* length */
			0,			/* TOS */
			242,			/* IP ID */
			0,			/* IP Frag */
			64,			/* TTL */
			IPPROTO_TCP,		/* protocol */
			0,			/* checksum */
			dst_v4,			/* source IP */
			src_v4,			/* destination IP */
			NULL,			/* payload */
			0,			/* payload size */
			l,			/* libnet handle */
			0);			/* libnet id */
	else {
		loge("Unknown socket family\n");
		goto err;
	}
	if (ret == -1) {
		loge("Can't build IP header: %s\n", libnet_geterror(l));
		goto err;
	}

	ret = libnet_write(l);
	if (ret == -1) {
		loge("Unable to send a fin packet: %s\n", libnet_geterror(l));
		goto err;
	}

	exit_code = 0;
err:
	libnet_destroy(l);
	return exit_code;
}

int send_queue(struct libsoccr_sk *sk, int queue, char *buf, __u32 len)
{
	logd("\tRestoring TCP %d queue data %u bytes\n", queue, len);

	if (setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		logerr("Can't set repair queue");
		return -1;
	}

	return __send_queue(sk, queue, buf, len);
}


int __send_queue(struct libsoccr_sk *sk, int queue, char *buf, __u32 len)
{
	int ret, err = -1, max_chunk;
	int off;

	max_chunk = len;
	off = 0;

	do {
		int chunk = len;

		if (chunk > max_chunk)
			chunk = max_chunk;

		ret = send(sk->fd, buf + off, chunk, 0);
		if (ret <= 0) {
			if (max_chunk > 1024) {
				/*
				 * Kernel not only refuses the whole chunk,
				 * but refuses to split it into pieces too.
				 *
				 * When restoring recv queue in repair mode
				 * kernel doesn't try hard and just allocates
				 * a linear skb with the size we pass to the
				 * system call. Thus, if the size is too big
				 * for slab allocator, the send just fails
				 * with ENOMEM.
				 *
				 * In any case -- try smaller chunk, hopefully
				 * there's still enough memory in the system.
				 */
				max_chunk >>= 1;
				continue;
			}

			logerr("Can't restore %d queue data (%d), want (%d:%d:%d)",
				  queue, ret, chunk, len, max_chunk);
			goto err;
		}
		off += ret;
		len -= ret;
	} while (len);

	err = 0;
err:
	return err;
}


int ipv6_addr_mapped(union libsoccr_addr *addr)
{
	return (addr->v6.sin6_addr.s6_addr32[2] == htonl(0x0000ffff));
}


int libsoccr_set_sk_data_noq_conn(struct libsoccr_sk *sk,
		struct sk_data_info *data, unsigned int data_size)
{
	struct tcp_repair_opt opts[4];
	int addr_size, mstate;
	int onr = 0;
	__u32 seq;

	if (!data || data_size < SOCR_DATA_MIN_SIZE) {
		printf("Invalid input parameters\n");
		return -1;
	}

	if (!sk->dst_addr || !sk->src_addr) {
		printf("Destination or/and source addresses aren't set\n");
		return -1;
	}

	mstate = 1 << data->sk_data.state;

	if (data->sk_data.state == TCP_LISTEN) {
		printf("Unable to handle listen sockets\n");
		return -1;
	}

	if (sk->src_addr->sa.sa_family == AF_INET)
		addr_size = sizeof(sk->src_addr->v4);
	else
		addr_size = sizeof(sk->src_addr->v6);

	if (bind(sk->fd, &sk->src_addr->sa, addr_size)) {
		printf("Can't bind inet socket back");
		return -1;
	}

	if (mstate & (RCVQ_FIRST_FIN | RCVQ_SECOND_FIN))
		data->sk_data.inq_seq--;

	/* outq_seq is adjusted due to not accointing the fin packet */
	if (mstate & (SNDQ_FIRST_FIN | SNDQ_SECOND_FIN))
		data->sk_data.outq_seq--;

	if (set_queue_seq(sk, TCP_RECV_QUEUE,
				data->sk_data.inq_seq - data->sk_data.inq_len))
		return -2;

	seq = data->sk_data.outq_seq - data->sk_data.outq_len;
	if (data->sk_data.state == TCP_SYN_SENT)
		seq--;

	if (set_queue_seq(sk, TCP_SEND_QUEUE, seq))
		return -3;

	if (sk->dst_addr->sa.sa_family == AF_INET)
		addr_size = sizeof(sk->dst_addr->v4);
	else
		addr_size = sizeof(sk->dst_addr->v6);

	if (data->sk_data.state == TCP_SYN_SENT && tcp_repair_off(sk->fd))
		return -1;

	if (connect(sk->fd, &sk->dst_addr->sa, addr_size) == -1 &&
						errno != EINPROGRESS) {
		printf("Can't connect inet socket back");
		return -1;
	}

	if (data->sk_data.state == TCP_SYN_SENT && tcp_repair_on(sk->fd))
		return -1;

	printf("\tRestoring TCP options\n");

	if (data->sk_data.opt_mask & TCPI_OPT_SACK) {
		printf("\t\tWill turn SAK on\n");
		opts[onr].opt_code = TCPOPT_SACK_PERM;
		opts[onr].opt_val = 0;
		onr++;
	}

	if (data->sk_data.opt_mask & TCPI_OPT_WSCALE) {
		printf("\t\tWill set snd_wscale to %u\n", data->sk_data.snd_wscale);
		printf("\t\tWill set rcv_wscale to %u\n", data->sk_data.rcv_wscale);
		opts[onr].opt_code = TCPOPT_WINDOW;
		opts[onr].opt_val = data->sk_data.snd_wscale + (data->sk_data.rcv_wscale << 16);
		onr++;
	}

	if (data->sk_data.opt_mask & TCPI_OPT_TIMESTAMPS) {
		printf("\t\tWill turn timestamps on\n");
		opts[onr].opt_code = TCPOPT_TIMESTAMP;
		opts[onr].opt_val = 0;
		onr++;
	}

	printf("Will set mss clamp to %u\n", data->sk_data.mss_clamp);
	opts[onr].opt_code = TCPOPT_MAXSEG;
	opts[onr].opt_val = data->sk_data.mss_clamp;
	onr++;

	if (data->sk_data.state != TCP_SYN_SENT &&
	    setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_OPTIONS,
				opts, onr * sizeof(struct tcp_repair_opt)) < 0) {
		printf("Can't repair options");
		return -2;
	}

	if (data->sk_data.opt_mask & TCPI_OPT_TIMESTAMPS) {
		if (setsockopt(sk->fd, SOL_TCP, TCP_TIMESTAMP,
				&data->sk_data.timestamp, sizeof(data->sk_data.timestamp)) < 0) {
			logerr("Can't set timestamp");
			return -3;
		}
	}

	return 0;
}

int libsoccr_restore_conn(struct libsoccr_sk *sk,
						  struct sk_data_info* data, 
						  unsigned int data_size)
{
	int mstate = 1 << data->sk_data.state;

	if (libsoccr_set_sk_data_noq_conn(sk, data, data_size))
		return -1;


	if (libsoccr_restore_queue_HA(sk, data, sizeof(*data), TCP_RECV_QUEUE, data->recv_queue))
		return -1;

	if (libsoccr_restore_queue_HA(sk, data, sizeof(*data), TCP_SEND_QUEUE, data->send_queue))
		return -1;

	if (data->sk_data.flags & SOCCR_FLAGS_WINDOW) {
		struct tcp_repair_window wopt = {
			.snd_wl1 = data->sk_data.snd_wl1,
			.snd_wnd = data->sk_data.snd_wnd,
			.max_window = data->sk_data.max_window,
			.rcv_wnd = data->sk_data.rcv_wnd,
			.rcv_wup = data->sk_data.rcv_wup,
		};

		if (mstate & (RCVQ_FIRST_FIN | RCVQ_SECOND_FIN)) {
			wopt.rcv_wup--;
			wopt.rcv_wnd++;
		}

		if (setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_WINDOW, &wopt, sizeof(wopt))) {
			logerr("Unable to set window parameters");
			return -1;
		}
	}

	/*
	 * To restore a half closed sockets, fin packets has to be restored in
	 * recv and send queues. Here shutdown() is used to restore a fin
	 * packet in the send queue and a fake fin packet is send to restore it
	 * in the recv queue.
	 */
	if (mstate & SNDQ_FIRST_FIN)
		restore_fin_in_snd_queue(sk->fd, mstate & SNDQ_FIN_ACKED);

	/* Send a fin packet to the socket to restore it in a receive queue. */
	if (mstate & (RCVQ_FIRST_FIN | RCVQ_SECOND_FIN))
		if (send_fin_HA(sk, data, data_size, TH_ACK | TH_FIN) < 0)
			return -1;

	if (mstate & SNDQ_SECOND_FIN)
		restore_fin_in_snd_queue(sk->fd, mstate & SNDQ_FIN_ACKED);

	if (mstate & RCVQ_FIN_ACKED)
		data->sk_data.inq_seq++;

	if (mstate & SNDQ_FIN_ACKED) {
		data->sk_data.outq_seq++;
		if (send_fin_HA(sk, data, data_size, TH_ACK) < 0)
			return -1;
	}


	return 0;
}
