#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <net/if.h>

#include "common.h"
#include "peer.h"
#include "acl.h"

#define MAX_EVENTS 8

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void handle_handshake(int32_t sock, const uint8_t *packet, int32_t n, const struct sockaddr_in *src, socklen_t srclen) {
	char ib[INET_ADDRSTRLEN];
	uint8_t res[sizeof(msg_header_t)];
	msg_header_t msg_header;
	struct in_addr tun_ip;
	peer_t *peer;

	if (n < (int32_t)(sizeof(msg_header_t) + sizeof(tun_ip))) {
		printf("[HS] short request (%d bytes)\n", n);
		return;
	}

	memcpy(&tun_ip, packet + sizeof(msg_header_t), sizeof(tun_ip));

	peer = peer_register(tun_ip, src, srclen);
	if (peer == NULL) {
		printf("[HS] register failed for %s\n", ip_str(tun_ip, ib));
		return;
	}

	memset(&msg_header, 0, sizeof(msg_header));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_RES_HANDSHAKE;
	msg_header.session_idx = peer->session_idx;
	msg_encode(res, sizeof(res), &msg_header);

	if (sendto(sock, res, sizeof(res), 0, (const struct sockaddr *)&peer->outer, peer->outer_len) < 0){
		perror("sendto handshake");
	}
}

static void send_keepalive(int32_t sock, const peer_t *peer){
	int32_t sent;
	int32_t off;
	uint8_t packet[sizeof(msg_header_t)];
	msg_header_t msg_header;

	memset(&msg_header, 0, sizeof(msg_header));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.session_idx = peer->session_idx;
	msg_header.type = MSG_TYPE_KEEPALIVE;

	off = msg_encode(packet, sizeof(packet), &msg_header);
	sent = sendto(sock, packet, off, 0,
			(const struct sockaddr *)&peer->outer, peer->outer_len);
	if (sent < 0) {
		perror("sendto");
	}
}

int main(int argc, char *argv[])
{
	char ifname[IFNAMSIZ];
	char ib[INET_ADDRSTRLEN], ob[INET_ADDRSTRLEN];
	int tun_fd, sock, n, sent, port;
	int epfd = -1, nev, i;
	int acl_enabled = 0;
	struct epoll_event ev, events[MAX_EVENTS];
	struct sockaddr_in local_addr;
	struct sockaddr_in src;
	socklen_t srclen;
	struct in_addr inner;
	peer_t *peer;
	struct in_addr inner_dst;
	uint8_t packet[BUF_SIZE];
	uint8_t *ip;
	msg_header_t msg_header;
	data_header_t data_header;
	int32_t off;
	char *end;
	int ret = 1;
	int fd = 0;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s <port> [acl-file]\n", argv[0]);
		return 1;
	}
	port = strtol(argv[1], &end, 10);
	if (*end != '\0' || port <= 0 || port > 65535) {
		fprintf(stderr, "invalid port : %s\n", argv[1]);
		return 1;
	}

	peer_init();
	acl_init();

	if (argc == 3) {
		int nr = acl_load(argv[2]);
		if (nr < 0) {
			fprintf(stderr, "acl_load failed: %s\n", argv[2]);
			return 1;
		}
		printf("acl: %d rules loaded from %s\n", nr, argv[2]);
		acl_dump();
		acl_enabled = 1;
	} else {
		printf("acl: disabled (no rule file)\n");
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	ifname[0] = '\0';
	tun_fd = tun_alloc(ifname);
	if (tun_fd < 0) {
		fprintf(stderr, "tun_alloc failed\n"); return 1;
	}

	printf("tun device: %s (fd=%d)\n", ifname, tun_fd);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket"); close(tun_fd); return 1;
	}

	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin_family      = AF_INET;
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	local_addr.sin_port        = htons((uint16_t)port);

	if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		perror("bind"); goto out;
	}

	printf("listening on UDP 0.0.0.0:%d\n", port);

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		perror("epoll_create1"); goto out;
	}

	ev.events  = EPOLLIN;
	ev.data.fd = tun_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, tun_fd, &ev) < 0) {
		perror("epoll_ctl: tun_fd"); goto out;
	}

	ev.events  = EPOLLIN;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0) {
		perror("epoll_ctl: sock"); goto out;
	}

	printf("epoll ready (epfd=%d, tun_fd=%d, sock=%d)\n", epfd, tun_fd, sock);

	while (running) {
		nev = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (nev < 0) {
			if (errno == EINTR){
				continue;
			}
			perror("epoll_wait");
			break;
		}

		for (i = 0; i < nev; i++) {
			fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fprintf(stderr, "[err] fd=%d events=0x%x\n",
						fd, events[i].events);
				continue;
			}

			if (fd == sock) {
				srclen = sizeof(src);
				n = recvfrom(sock, packet, sizeof(packet), 0,
						(struct sockaddr *)&src, &srclen);
				if (n < 0) {
					perror("recvfrom"); goto out;
				}


				off = msg_decode(packet, (size_t)n, &msg_header);
				if (off < 0 || msg_header.version != PROTOCOL_VERSION) {
					printf("[drop] bad header (%d bytes)\n", n);
					continue;
				}

				if (msg_header.type == MSG_TYPE_REQ_HANDSHAKE) {
					handle_handshake(sock, packet, n, &src, srclen);
					continue;
				}

				peer = peer_find_idx(msg_header.session_idx);
				if (peer == NULL) {
					printf("[drop] unknown session_idx[%08x]\n", msg_header.session_idx);
					continue;
				}

				switch (msg_header.type) {
					case MSG_TYPE_DATA:
						break;
					case MSG_TYPE_KEEPALIVE:
						peer_touch(peer, &src, srclen);
						send_keepalive(sock, peer);
						continue;
					default:
						printf("[drop] unknown type %u\n", msg_header.type);
						continue;
				}

				if (data_decode(packet + off, (size_t)(n - off), &data_header) < 0) {
					continue;
				}

				off += sizeof(data_header_t);
				n -= off;

				ip = packet + off;
				if (n < IP_MIN_HDR || IP_VERSION(ip) != 4){
					continue;
				}

				memcpy(&inner, ip + IP_SRC_OFF, IP_ADDR_LEN);

				if (inner.s_addr != peer->inner.s_addr) {
					printf("[drop] spoofed src %s for idx %08x\n", ip_str(inner, ib), peer->session_idx);
					continue;
				}

				if (acl_enabled && acl_check(ip, n) != ACL_ALLOW) {
					memcpy(&inner_dst, ip + IP_DST_OFF, IP_ADDR_LEN);
					printf("[deny] %s -> %s proto=%d (%d bytes)\n",
							ip_str(inner, ib), ip_str(inner_dst, ob),
							ip[IP_PROTO_OFF], n);
					continue;
				}

				peer_touch(peer, &src, srclen);

				printf("[udp->tun] %d bytes from %s:%d (inner src %s), icmp type=%d\n",
						n, ip_str(src.sin_addr, ob), ntohs(src.sin_port),
						ip_str(inner, ib), ip[IP_IHL(ip)]);

				if (write(tun_fd, ip, n) < 0) {
					perror("write tun");
				}
			}
			else if (fd == tun_fd) {
				n = read(tun_fd, packet + PKT_HDR_LEN, sizeof(packet) - PKT_HDR_LEN);
				if (n < 0) { perror("read tun"); goto out; }

				ip = packet + PKT_HDR_LEN;
				if (n < IP_MIN_HDR || IP_VERSION(ip) != 4) {
					printf("[skip] non-IPv4 (ver=%d, %d bytes)\n",
							IP_VERSION(ip), n);
					continue;
				}

				memcpy(&inner, ip + IP_DST_OFF, IP_ADDR_LEN);

				memset(&msg_header, 0, sizeof(msg_header));

				peer = peer_lookup(inner);
				if (peer == NULL) {
					printf("[drop] no peer for %s\n", ip_str(inner, ib));
					continue;
				}

				printf("[tun->udp] %d bytes to %s (%s:%d), icmp type=%d\n",
						n, ip_str(inner, ib),
						ip_str(peer->outer.sin_addr, ob), ntohs(peer->outer.sin_port),
						ip[IP_IHL(ip)]);

				msg_header.version = PROTOCOL_VERSION;
				msg_header.type = MSG_TYPE_DATA;
				msg_header.session_idx = peer->session_idx;
				data_header.counter = peer->tx_counter++;

				off = msg_encode(packet, sizeof(packet), &msg_header);
				off += data_encode(packet + off, sizeof(packet) - off, &data_header);

				sent = sendto(sock, packet, off + n, 0,
						(struct sockaddr *)&peer->outer, peer->outer_len);
				if (sent < 0) {
					perror("sendto");
				}
			}
		}
	}
	ret = 0;

out:
	if (epfd >= 0) {
		close(epfd);
	}
	close(sock);
	close(tun_fd);
	return ret;
}

