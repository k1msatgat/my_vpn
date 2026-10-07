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

int main(int argc, char *argv[])
{
	char ifname[IFNAMSIZ];
	char ib[INET_ADDRSTRLEN], ob[INET_ADDRSTRLEN];
	int tun_fd, sock, n, sent, port;
	int epfd, nev, i;
	int acl_enabled = 0;
	struct epoll_event ev, events[MAX_EVENTS];
	struct sockaddr_in local_addr, src;
	socklen_t srclen;
	struct in_addr inner;
	peer_t *p;
	unsigned char buf[BUF_SIZE];

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s <port> [acl-file]\n", argv[0]);
		return 1;
	}
	port = atoi(argv[1]);

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
	local_addr.sin_port        = htons(port);

	if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		perror("bind"); close(sock); close(tun_fd); return 1;
	}

	printf("listening on UDP 0.0.0.0:%d\n", port);

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		perror("epoll_create1"); close(sock); close(tun_fd); return 1;
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
			int fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fprintf(stderr, "[err] fd=%d events=0x%x\n",
						fd, events[i].events);
				continue;
			}

			if (fd == sock) {
				srclen = sizeof(src);
				n = recvfrom(sock, buf, sizeof(buf), 0,
						(struct sockaddr *)&src, &srclen);
				if (n < 0) {
					perror("recvfrom"); goto out;
				}

				if (n < IP_MIN_HDR || IP_VERSION(buf) != 4) {
					printf("[drop] udp: not IPv4 (%d bytes)\n", n);
					continue;
				}
				memcpy(&inner, buf + IP_SRC_OFF, IP_ADDR_LEN);
				peer_learn(inner, &src, srclen);

				if (acl_enabled && acl_check(buf, n) != ACL_ALLOW) {
					struct in_addr inner_dst;
					memcpy(&inner_dst, buf + IP_DST_OFF, IP_ADDR_LEN);
					printf("[deny] %s -> %s proto=%d (%d bytes)\n",
							ip_str(inner, ib), ip_str(inner_dst, ob),
							buf[IP_PROTO_OFF], n);
					continue;
				}

				printf("[udp->tun] %d bytes from %s:%d (inner src %s), icmp type=%d\n",
						n, ip_str(src.sin_addr, ob), ntohs(src.sin_port),
						ip_str(inner, ib), buf[IP_IHL(buf)]);

				if (write(tun_fd, buf, n) < 0) {
					perror("write tun");
				}
			}
			else if (fd == tun_fd) {
				n = read(tun_fd, buf, sizeof(buf));
				if (n < 0) { perror("read tun"); goto out; }

				if (n < IP_MIN_HDR || IP_VERSION(buf) != 4) {
					printf("[skip] non-IPv4 (ver=%d, %d bytes)\n",
							IP_VERSION(buf), n);
					continue;
				}

				memcpy(&inner, buf + IP_DST_OFF, IP_ADDR_LEN);

				p = peer_lookup(inner);
				if (p == NULL) {
					printf("[drop] no peer for %s\n", ip_str(inner, ib));
					continue;
				}

				printf("[tun->udp] %d bytes to %s (%s:%d), icmp type=%d\n",
						n, ip_str(inner, ib),
						ip_str(p->outer.sin_addr, ob), ntohs(p->outer.sin_port),
						buf[IP_IHL(buf)]);

				sent = sendto(sock, buf, n, 0,
						(struct sockaddr *)&p->outer, p->outer_len);
				if (sent < 0) {
					perror("sendto");
				}
			}
		}
	}

out:
	close(epfd);
	close(sock);
	close(tun_fd);
	return 0;
}

