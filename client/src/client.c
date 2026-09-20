#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <net/if.h>

#include "common.h"

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
	int tun_fd, sock, n, sent;
	int epfd, nev, i;
	struct epoll_event ev;
	struct epoll_event events[MAX_EVENTS];
	struct sockaddr_in peer;
	unsigned char buf[BUF_SIZE];

	if (argc != 4){
		fprintf(stderr, "usage: %s <ifname> <server-ip> <port>\n", argv[0]);
		fprintf(stderr, " e.g. %s tun0 49.247.139.39 9000\n", argv[0]);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	strncpy(ifname, argv[1], IFNAMSIZ - 1);
	ifname[IFNAMSIZ - 1] = '\0';

	tun_fd = tun_alloc(ifname);
	if (tun_fd < 0){
		fprintf(stderr, "tun_alloc failed\n");
		return 1;
	}

	printf("[%s] tun device ready (fd=%d)\n", ifname, tun_fd);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		close(tun_fd);
		return 1;
	}

	memset(&peer, 0, sizeof(peer));
	peer.sin_family = AF_INET;
	peer.sin_port = htons(atoi(argv[3]));
	if (inet_pton(AF_INET, argv[2], &peer.sin_addr) != 1){
		fprintf(stderr, "bad server address : %s\n", argv[2]);
		close(sock);
		close(tun_fd);
		return 1;
	}

	printf("[%s] tunneling to %s:%s\n", ifname, argv[2], argv[3]);

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		perror("epoll_create1");
		close(sock);
		close(tun_fd);
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = tun_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, tun_fd, &ev) < 0){
		perror("epoll_ctl: sock");
		close(epfd);
		close(sock);
		close(tun_fd);
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0){
		perror("epoll_ctl: sock");
		close(epfd);
		close(sock);
		close(tun_fd);
		return 1;
	}

	printf("[%s] epoll ready (epfd=%d, tun_fd=%d, sock=%d)\n", ifname, epfd, tun_fd, sock);

	while (running){
		nev = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (nev < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("epoll_wait");
			break;
		}

		for (i=0;i<nev;i++){
			int fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fprintf(stderr, "[%s][err] fd=%d evnets=0x%x\n", ifname, fd, events[i].events);
				continue;
			}

			if (fd == tun_fd) {
				n = read(tun_fd, buf, sizeof(buf));
				if (n < 0) {
					perror("read tun");
					goto out;
				}

				if (n < 20 || (buf[0] >> 4) != 4) {
					printf("[%s][skip] non-IPv4 (ver=%d, %d bytes)\n", ifname, buf[0] >> 4, n);
				}
				else{
					printf("[%s][tun->udp] %d bytes, proto=%d, icmp type=%d\n", ifname, n, buf[9], buf[20]);
					sent = sendto(sock, buf, n, 0, (struct sockaddr *)&peer, sizeof(peer));
					if (sent < 0) perror("sendto");
				}

			}
			else if(fd == sock) {
				n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
				if ( n < 0) {
					perror("recvfrom"); goto out;
				}

				if (n < 20 || (buf[0] >> 4) != 4) {
					printf("[%s][drop] udp: not IPv4 (%d bytes)\n", ifname, n);
					continue;
				}

				printf("[%s][udp->tun] %d bytes, proto=%d, icmp type=%d\n", ifname, n, buf[9], buf[20]);

				if (write(tun_fd, buf, n) < 0) {
					perror("write tun");
				}
			}
		}
	}

out :
	close(epfd);
	close(sock);
	close(tun_fd);

	return 0;
}

