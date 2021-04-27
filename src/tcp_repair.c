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

#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <assert.h>



#define DEBUG_TCP_REPAIR 1
#if DEBUG_TCP_REPAIR
#define TCPR_PRINTF(x...) printf(x)
#else
#define TCPR_PRINTF(x...)
#endif


#if USING_SOCCR_LOG
unsigned int log_level = 8;
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
		printf("Can't turn TCP repair mode ON\n");

	return ret;
}

int tcp_repair_off(int fd)
{
	int aux = 0, ret;

	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR, &aux, sizeof(aux));
	if (ret < 0)
		printf("Failed to turn off repair mode on socket\n");

	return ret;
}


int set_queue_seq(struct libsoccr_sk *sk, int queue, __u32 seq)
{
	int ret = 0;
	printf("\tSetting %d queue seq to %u\n", queue, seq);

	ret = setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue));

	if (ret < 0) {
		printf("Can't set repair queue (%d)\n", ret);
		return -1;
	}


	ret = setsockopt(sk->fd, SOL_TCP, TCP_QUEUE_SEQ, &seq, sizeof(seq));

	if (ret < 0) {
		printf("Can't set queue seq (%d)\n", ret);
		return -1;
	}

	return 0;
}

int libsoccr_restore_queue_HA(struct libsoccr_sk *sk, struct sk_data_info *data, unsigned data_size,
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
		printf("Can't set repair queue\n");
		return -1;
	}

	ret = shutdown(sk, SHUT_WR);
	if (ret < 0)
		printf("Unable to shut down a socket\n");

	queue = TCP_NO_QUEUE;
	if (acked &&
	    setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		printf("Can't set repair queue\n");
		return -1;
	}

	return ret;
}

int send_fin_HA(struct libsoccr_sk *sk, struct sk_data_info *data,
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
		printf("libnet_init failed (%s)\n", errbuf);
		return -1;
	}

	if (setsockopt(l->fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark))) {
		printf("Can't set SO_MARK (%d) for socket\n", mark);
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
		printf("Can't build TCP header: %s\n", libnet_geterror(l));
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
		printf("Unknown socket family\n");
		goto err;
	}
	if (ret == -1) {
		printf("Can't build IP header: %s\n", libnet_geterror(l));
		goto err;
	}

	ret = libnet_write(l);
	if (ret == -1) {
		printf("Unable to send a fin packet: %s\n", libnet_geterror(l));
		goto err;
	}

	exit_code = 0;
err:
	libnet_destroy(l);
	return exit_code;
}

int send_queue(struct libsoccr_sk *sk, int queue, char *buf, __u32 len)
{
	printf("\tRestoring TCP %d queue data %u bytes\n", queue, len);

	if (setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		printf("Can't set repair queue\n");
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

			printf("Can't restore %d queue data (%d), want (%d:%d:%d)\n",
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
	int ret = 0;
	struct sockaddr_in temp_v4;
	struct sockaddr_in6 temp_v6;


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
		printf("Can't bind inet socket back\n");
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

	if (sk->dst_addr->sa.sa_family == AF_INET) {
		addr_size = sizeof(sk->dst_addr->v4); 
		memcpy(&temp_v4, &sk->dst_addr->sa, addr_size);
		temp_v4.sin_family = AF_UNSPEC;
		connect(sk->fd, &temp_v4, addr_size);
	}
	else {
		addr_size = sizeof(sk->dst_addr->v6);
		memcpy(&temp_v6, &sk->dst_addr->sa, addr_size);
		temp_v6.sin6_family = AF_UNSPEC;
		connect(sk->fd, &temp_v6, addr_size);
	}


	if (data->sk_data.state == TCP_SYN_SENT && tcp_repair_off(sk->fd)) {
		printf("Failed data->sk_data.state == TCP_SYN_SENT && tcp_repair_off(sk->fd)\n");
		return -1;
	}

	if (connect(sk->fd, &sk->dst_addr->sa, addr_size) == -1 &&
						errno != EINPROGRESS) {
		printf("Can't connect inet socket back\n");
		return -1;
	}

	if (data->sk_data.state == TCP_SYN_SENT && tcp_repair_on(sk->fd)) {
		printf("Failed data->sk_data.state == TCP_SYN_SENT && tcp_repair_on(sk->fd)\n");
		return -1;
	}

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
	printf("Will set sk_data state to %u\n", data->sk_data.state);
	opts[onr].opt_code = TCPOPT_MAXSEG;
	opts[onr].opt_val = data->sk_data.mss_clamp;
	onr++;

	if (data->sk_data.state != TCP_SYN_SENT) {
	    ret = setsockopt(sk->fd, SOL_TCP, TCP_REPAIR_OPTIONS, opts, 
						 onr * sizeof(struct tcp_repair_opt));

		if (ret < 0) {					
			printf("Can't repair options result:%d\n", ret);
			return -2;
		}
	}

	if (data->sk_data.opt_mask & TCPI_OPT_TIMESTAMPS) {
		if (setsockopt(sk->fd, SOL_TCP, TCP_TIMESTAMP,
				&data->sk_data.timestamp, sizeof(data->sk_data.timestamp)) < 0) {
			printf("Can't set timestamp\n");
			return -3;
		}
	}

	return 0;
}

int libsoccr_restore_conn(struct sk_data_info* data, 
						  unsigned int data_size)
{
	int mstate = 1 << data->sk_data.state;

	printf("[%s] Enter\n", __func__);

	if (libsoccr_set_sk_data_noq_conn(data->libsoccr_sk, data, data_size))
		return -1;

	if (libsoccr_restore_queue_HA(data->libsoccr_sk, data, sizeof(*data), TCP_RECV_QUEUE, data->libsoccr_sk->recv_queue))
		return -1;

	if (libsoccr_restore_queue_HA(data->libsoccr_sk, data, sizeof(*data), TCP_SEND_QUEUE, data->libsoccr_sk->send_queue))
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

		if (setsockopt(data->libsoccr_sk->fd, SOL_TCP, TCP_REPAIR_WINDOW, &wopt, sizeof(wopt))) {
			printf("Unable to set window parameters\n");
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
		restore_fin_in_snd_queue(data->libsoccr_sk->fd, mstate & SNDQ_FIN_ACKED);

	/* Send a fin packet to the socket to restore it in a receive queue. */
	if (mstate & (RCVQ_FIRST_FIN | RCVQ_SECOND_FIN))
		if (send_fin_HA(data->libsoccr_sk, data, data_size, TH_ACK | TH_FIN) < 0)
			return -1;

	if (mstate & SNDQ_SECOND_FIN)
		restore_fin_in_snd_queue(data->libsoccr_sk->fd, mstate & SNDQ_FIN_ACKED);

	if (mstate & RCVQ_FIN_ACKED)
		data->sk_data.inq_seq++;

	if (mstate & SNDQ_FIN_ACKED) {
		data->sk_data.outq_seq++;
		if (send_fin_HA(data->libsoccr_sk, data, data_size, TH_ACK) < 0)
			return -1;
	}

	printf("[%s] Exit\n", __func__);

	return 0;
}
static int refresh_sk(struct libsoccr_sk *sk,
			struct libsoccr_sk_data *data, struct soccr_tcp_info *ti)
{
	int size;
	socklen_t olen = sizeof(*ti);

	if (getsockopt(sk->fd, SOL_TCP, TCP_INFO, ti, &olen) || olen != sizeof(*ti)) {
		printf("Failed to obtain TCP_INFO FD:%d\n", sk->fd);
		return -1;
	}

	switch (ti->tcpi_state) {
	case TCP_ESTABLISHED:
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:
	case TCP_LAST_ACK:
	case TCP_CLOSE_WAIT:
	case TCP_CLOSING:
	case TCP_CLOSE:
	case TCP_SYN_SENT:
		break;
	default:
		printf("Unknown state %d\n", ti->tcpi_state);
		return -1;
	}

	data->state = ti->tcpi_state;

	if (ioctl(sk->fd, SIOCOUTQ, &size) == -1) {
		printf("Unable to get size of snd queue\n");
		return -1;
	}

	data->outq_len = size;

	if (ioctl(sk->fd, SIOCOUTQNSD, &size) == -1) {
		printf("Unable to get size of unsent data\n");
		return -1;
	}

	data->unsq_len = size;

	if (data->state == TCP_CLOSE) {
		/* A connection could be reseted. In thise case a sent queue
		 * may contain some data. A user can't read this data, so let's
		 * ignore them. Otherwise we will need to add a logic whether
		 * the send queue contains a fin packet or not and decide whether
		 * a fin or reset packet has to be sent to restore a state
		 */

		data->unsq_len = 0;
		data->outq_len = 0;
	}

	/* Don't account the fin packet. It doesn't countain real data. */
	if ((1 << data->state) & (SNDQ_FIRST_FIN | SNDQ_SECOND_FIN)) {
		printf("in data->outq_len--\n");
		if (data->outq_len)
			data->outq_len--;
		data->unsq_len = data->unsq_len ? data->unsq_len - 1 : 0;
	}

	if (ioctl(sk->fd, SIOCINQ, &size) == -1) {
		printf("Unable to get size of recv queue\n");
		return -1;
	}

	data->inq_len = size;

	return 0;
}

static int get_stream_options(struct libsoccr_sk *sk,
		struct libsoccr_sk_data *data, struct soccr_tcp_info *ti)
{
	int ret;
	socklen_t auxl;
	int val;

	auxl = sizeof(data->mss_clamp);
	ret = getsockopt(sk->fd, SOL_TCP, TCP_MAXSEG, &data->mss_clamp, &auxl);
	if (ret < 0) {
		printf("Get TCP_MAXSEG Failed (%d)\n", ret);
		goto err_sopt;
	}

	data->opt_mask = ti->tcpi_options;
	if (ti->tcpi_options & TCPI_OPT_WSCALE) {
		data->snd_wscale = ti->tcpi_snd_wscale;
		data->rcv_wscale = ti->tcpi_rcv_wscale;
	}

	if (ti->tcpi_options & TCPI_OPT_TIMESTAMPS) {
		auxl = sizeof(val);
		ret = getsockopt(sk->fd, SOL_TCP, TCP_TIMESTAMP, &val, &auxl);
		if (ret < 0) {
			printf("Get TCP_TIMESTAMP Failed (%d)\n", ret);
			goto err_sopt;
		}

		data->timestamp = val;
	}

	return 0;

err_sopt:
	printf("\tsockopt failed\n");
	return -1;
}

static int get_window(struct libsoccr_sk *sk, struct libsoccr_sk_data *data)
{
	struct tcp_repair_window opt;
	socklen_t optlen = sizeof(opt);

	if (getsockopt(sk->fd, SOL_TCP,
			TCP_REPAIR_WINDOW, &opt, &optlen)) {
		/* Appeared since 4.8, but TCP_repair itself is since 3.11 */
		if (errno == ENOPROTOOPT)
			return 0;

		printf("Unable to get window properties FD:%d\n", sk->fd);
		return -1;
	}

	data->flags |= SOCCR_FLAGS_WINDOW;
	data->snd_wl1		= opt.snd_wl1;
	data->snd_wnd		= opt.snd_wnd;
	data->max_window	= opt.max_window;
	data->rcv_wnd		= opt.rcv_wnd;
	data->rcv_wup		= opt.rcv_wup;

	return 0;
}

#if 0
struct tcp_pipe *get_tcp_pipe()
{
	struct tcp_pipe *ret = NULL;
	int pipefd[2];

	ret = calloc(1, sizeof(struct tcp_pipe));

	if (pipe(pipefd) < 0) {
		goto out;
	}

	ret->data = 0;
	ret->prod = pipefd[1];
	ret->cons = pipefd[0];
	ret->next = NULL;

	return ret;

out:
	if (ret != NULL) {
		free(ret);
	}

	return NULL;
}

static inline void kill_tcp_pipe(struct tcp_pipe *p)
{
	close(p->prod);
	close(p->cons);

	p->data = 0;
	p->prod = 0;
	p->cons = 0;

	return;
}
#endif
int get_queue(int sk, int queue_id,
		__u32 *seq, __u32 len, char **bufp)
{
	int ret, aux;
	socklen_t auxl;
	char *buf;

	aux = queue_id;
	auxl = sizeof(aux);
	ret = setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &aux, auxl);
	if (ret < 0)
		goto err_sopt;

	auxl = sizeof(*seq);
	ret = getsockopt(sk, SOL_TCP, TCP_QUEUE_SEQ, seq, &auxl);
	if (ret < 0)
		goto err_sopt;

	if (len) {
		/*
		 * Try to grab one byte more from the queue to
		 * make sure there are len bytes for real
		 */
		buf = malloc(len + 1);
		if (!buf) {
			printf("Unable to allocate memory\n");
			goto err_buf;
		}

		ret = recv(sk, buf, len + 1, MSG_PEEK | MSG_DONTWAIT);
		//printf("ret=:%d\n",ret);
		if (ret != len) {
			printf("[%s] ret != len \n", __func__);
			goto err_recv;
		}
	} else
		buf = NULL;
	//printf("!!!!!!!!!!!!!!!!%s",buf);
	*bufp = buf;
	return 0;

err_sopt:
	printf("\tsockopt failed\n");
err_buf:
	return -1;

err_recv:
	printf("\trecv failed (%d, want %d)\n", ret, len);
	//free(buf);
	goto err_buf;
}


int get_seq_ack(int sk, int queue_id,
		__u32 *seq, __u32 len)
{
	int ret, aux;
	socklen_t auxl;

	aux = queue_id;
	auxl = sizeof(aux);
	ret = setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &aux, auxl);
	if (ret < 0) {	
		printf("Get TCP_REPAIR_QUEUE Failed (%d)\n", ret);
		goto err_sopt;
	}
	
	auxl = sizeof(*seq);
	ret = getsockopt(sk, SOL_TCP, TCP_QUEUE_SEQ, seq, &auxl);
	if (ret < 0) {
		printf("Get TCP_QUEUE_SEQ Failed (%d)\n", ret);
		goto err_sopt;
	}

	return 0;

err_sopt:
	printf("\tsockopt failed\n");

	return -1;
}

#define SOCCR_SAVE_DATA 		0x00000001
#define SOCCR_SAVE_REFRESH 		0x00000002
#define SOCCR_SAVE_GET_STREAM	0x00000004
#define SOCCR_SAVE_GET_WINDOW	0x00000008
#define SOCCR_SAVE_RECV_QUEUE	0x00000010
#define SOCCR_SAVE_SEND_QUEUE	0x00000020

int libsoccr_save_zerocopy(struct libsoccr_sk *sk, struct libsoccr_sk_data *data, 
						   unsigned data_size)
{
	struct soccr_tcp_info ti;
	int err_send_queue = 0;
	int err_recv_queue = 0;
	int ret = 0;

	if (!data || data_size < SOCR_DATA_MIN_SIZE) {
		printf("Invalid input parameters\n");
		ret |= SOCCR_SAVE_DATA;
	}

	memset(data, 0, data_size);

	if (refresh_sk(sk, data, &ti)) {
		ret |= SOCCR_SAVE_REFRESH;
	}

	if (get_stream_options(sk, data, &ti)) {
		ret |= SOCCR_SAVE_GET_STREAM;
	}

	if (get_window(sk, data)) {
		ret |= SOCCR_SAVE_GET_WINDOW;
	}
	
	if (ret == 0) {
		sk->flags |= SK_FLAG_FREE_SQ | SK_FLAG_FREE_RQ;

#if 1 //USING_RQ_RECOVERY
		if (get_seq_ack(sk->fd, TCP_RECV_QUEUE, &data->inq_seq, data->inq_len)) {
			ret |= SOCCR_SAVE_RECV_QUEUE;
			err_send_queue = 1;
		}

		if (get_seq_ack(sk->fd, TCP_SEND_QUEUE, &data->outq_seq, data->outq_len)) {
			ret |= SOCCR_SAVE_SEND_QUEUE;
			err_recv_queue = 1; 
		}
#else
		release_sk(sk);

		if (get_queue(sk->fd, TCP_RECV_QUEUE, &data->inq_seq, data->inq_len, &sk->recv_queue)) {
			ret |= SOCCR_SAVE_RECV_QUEUE;
			err_recv_queue = 1; 
		}
		
		if (get_queue(sk->fd, TCP_SEND_QUEUE, &data->outq_seq, data->outq_len, &sk->send_queue)) {
			ret |= SOCCR_SAVE_SEND_QUEUE;
			err_send_queue = 1;
		}

#endif
		
		//printf("Send SEQ (%d): %u Length:%d\n", err_send_queue, data->outq_seq, data->outq_len);
		//printf("Recv ACK (%d): %u Length:%d\n", err_recv_queue, data->inq_seq, data->inq_len);

	}
	else {
		printf("Can't Get Queue Info\n");
	}


#if 0
	if (data->outq_len && err_send_queue == 0) {
		printf("######################## SEND ########################\n");
		for (int idx = 0; idx < data->outq_len; idx++ ) {
			
			printf("%02x", *(unsigned char*)(sk->send_queue + idx));
		
			if (idx % 64 == 63)
				printf("\n");
			else if (idx % 4 == 3)
				printf(" ");
		}
		printf("\n######################## #END ########################\n");
	}
#endif
#if 0
	if (data->inq_len && err_recv_queue == 0) {
		printf("\n######################## RECV ########################\n");

		for (int idx = 0; idx < data->inq_len; idx++ ) {
			
			printf("%02x", *(unsigned char*)(sk->recv_queue + idx));
			
			if (idx % 64 == 63)
				printf("\n");
			else if (idx % 4 == 3)
				printf(" ");	
		}

		printf("\n######################## #END ########################\n");
	}
	else {
		printf("\n######################## RECV ZERO ########################\n");
	}
#endif 

	return ret;

	//return sizeof(struct libsoccr_sk_data);
}

int libsoccr_restore_zerocopy(struct sk_data_info* data, 
						  unsigned int data_size)
{
	int mstate = 1 << data->sk_data.state;

	if (libsoccr_set_sk_data_noq_conn(data->libsoccr_sk, data, data_size))
		return -1;

	if (libsoccr_restore_queue_HA(data->libsoccr_sk, data, sizeof(*data), TCP_RECV_QUEUE, data->libsoccr_sk->recv_queue))
		return -1;

	if (libsoccr_restore_queue_HA(data->libsoccr_sk, data, sizeof(*data), TCP_SEND_QUEUE, data->libsoccr_sk->send_queue))
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

		if (setsockopt(data->libsoccr_sk->fd, SOL_TCP, TCP_REPAIR_WINDOW, &wopt, sizeof(wopt))) {
			printf("Unable to set window parameters\n");
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
		restore_fin_in_snd_queue(data->libsoccr_sk->fd, mstate & SNDQ_FIN_ACKED);

	/* Send a fin packet to the socket to restore it in a receive queue. */
	if (mstate & (RCVQ_FIRST_FIN | RCVQ_SECOND_FIN))
		if (send_fin_HA(data->libsoccr_sk, data, data_size, TH_ACK | TH_FIN) < 0)
			return -1;

	if (mstate & SNDQ_SECOND_FIN)
		restore_fin_in_snd_queue(data->libsoccr_sk->fd, mstate & SNDQ_FIN_ACKED);

	if (mstate & RCVQ_FIN_ACKED)
		data->sk_data.inq_seq++;

	if (mstate & SNDQ_FIN_ACKED) {
		data->sk_data.outq_seq++;
		if (send_fin_HA(data->libsoccr_sk, data, data_size, TH_ACK) < 0)
			return -1;
	}


	return 0;
}

void libsoccr_release_conn(struct libsoccr_sk *sk)
{
	if (sk->flags & SK_FLAG_FREE_RQ)
	{
		free(sk->recv_queue);
		sk->recv_queue = NULL;
	}
	if (sk->flags & SK_FLAG_FREE_SQ)
	{
		free(sk->send_queue);
		sk->send_queue = NULL;
	}

#if 0	
	if (sk->flags & SK_FLAG_FREE_SA)
	{
		free(sk->src_addr);
		sk->src_addr = NULL;
	}
	if (sk->flags & SK_FLAG_FREE_DA)
	{
		free(sk->dst_addr);
		sk->dst_addr = NULL;
	}
#endif	
	//free(sk);
	//sk = NULL;
}

int dump_tcp_number(int fd, u_int32_t *seq_number, u_int32_t *seq_length,
					u_int32_t *ack_number, u_int32_t *ack_length)
{
	int ret = 0;
	int aux = 0;
	socklen_t auxl;

	printf("[%s] FD:%d\n", __func__, fd);

	if (tcp_repair_on(fd) < 0) {
		printf("tcp_repair_on() failed (FD:%d)\n", fd);
		return -1;
	}

	if (ioctl(fd, SIOCOUTQ, seq_length) == -1) {
		printf("Unable to get size of snd queue\n");
		return -1;
	}

	aux = TCP_SEND_QUEUE;
	auxl = sizeof(aux);
	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR_QUEUE, &aux, auxl);
	if (ret < 0) {	
		printf("Get TCP_REPAIR_QUEUE Failed (%d)\n", ret);
		return -1;
	}
	
	auxl = sizeof(*seq_number);
	ret = getsockopt(fd, SOL_TCP, TCP_QUEUE_SEQ, seq_number, &auxl);
	if (ret < 0) {
		printf("Get TCP_QUEUE_SEQ Failed (%d)\n", ret);
		return -1;
	}


	if (ioctl(fd, SIOCINQ, &ack_length) == -1) {
		printf("Unable to get size of recv queue\n");
		return -1;
	}

	aux = TCP_RECV_QUEUE;
	auxl = sizeof(aux);
	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR_QUEUE, &aux, auxl);
	if (ret < 0) {	
		printf("Get TCP_REPAIR_QUEUE Failed (%d)\n", ret);
		return -1;
	}
	
	auxl = sizeof(*ack_number);
	ret = getsockopt(fd, SOL_TCP, TCP_QUEUE_SEQ, ack_number, &auxl);
	if (ret < 0) {
		printf("Get TCP_QUEUE_SEQ Failed (%d)\n", ret);
		return -1;
	}


	if (tcp_repair_off(fd) < 0) {
		printf("tcp_repair_off fail.\n");
		return -1;
	}

	ret = 1;

	return ret;
}


int show_sk_data_info(struct sk_data_info *ski_data) 
{
	//printf("[Source] IP:%08x\n", ski_data->sk_addr.src_addr);
	//printf("[Source] Port:%04x\n", ski_data->sk_addr.src_port);
	//printf("[Dest] IP:%08x\n", ski_data->sk_addr.dst_addr);
	//sDest] Port:%04x\n", ski_data->sk_addr.dst_port);
	
	printf("[TCP] state: %u\n", ski_data->sk_data.state);
	printf("[TCP] inq_seq: %u\n", ski_data->sk_data.inq_seq);
	printf("[TCP] inq_len: %u\n", ski_data->sk_data.inq_len);
	printf("[TCP] outq_seq: %u\n", ski_data->sk_data.outq_seq);
	printf("[TCP] outq_len: %u\n", ski_data->sk_data.outq_len);
	printf("[TCP] unsq_len: %u\n", ski_data->sk_data.unsq_len);



	printf("socr->src_addr IP:%04x\n", ski_data->libsoccr_sk->src_addr->v4.sin_addr.s_addr);
	printf("socr->src_addr Port:%04x\n", ski_data->libsoccr_sk->src_addr->v4.sin_port);
	printf("socr->dst_addr IP:%04x\n", ski_data->libsoccr_sk->dst_addr->v4.sin_addr.s_addr);
	printf("socr->dst_addr Port:%04x\n", ski_data->libsoccr_sk->dst_addr->v4.sin_port);


	return 0;
}