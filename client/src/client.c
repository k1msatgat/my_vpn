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

struct session {
	uint32_t idx;
	uint64_t tx_counter;
	struct sockaddr_in server;
};

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
	int32_t fd = 0;

	uint8_t packet[BUF_SIZE];
	msg_header_t msg_header;
	data_header_t data_header;
	uint64_t tx_counter = 0;
	int off = 0;


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
			fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fprintf(stderr, "[%s][err] fd=%d evnets=0x%x\n", ifname, fd, events[i].events);
				continue;
			}

			if (fd == tun_fd) {
				n = read(tun_fd, packet + PKT_HDR_LEN, sizeof(packet) - PKT_HDR_LEN);
				if (n < 0) {
					perror("read tun");
					goto out;
				}

				if (n < IP_MIN_HDR || IP_VERSION(packet) != 4) {
					printf("[%s][skip] non-IPv4 (ver=%d, %d bytes)\n", ifname, IP_VERSION(packet), n);
				}
				else {
					printf("[%s][tun->udp] %d bytes, proto=%d, icmp type=%d\n", ifname, n, packet[IP_PROTO_OFF], packet[IP_IHL(packet)]);
					memset(&msg_header,0, sizeof(msg_header_t));
					msg_header.version = 0; // 첫 버전으로 버전 관리 규칙이 생기기전까지 우선 0 을 사용한다.
					msg_header.type = MSG_TYPE_DATA;
					msg_header.session_idx = 0;
					data_header.counter = tx_counter++;

					off = msg_encode(packet, sizeof(packet), &msg_header);
					off += data_encode(packet+off, sizeof(packet) - off, &data_header);

					sent = sendto(sock, packet, off, 0, (struct sockaddr *)&peer, sizeof(peer));
					if (sent < 0) perror("sendto");
				}

			}
			else if(fd == sock) {
				n = recvfrom(sock, packet, sizeof(packet), 0, NULL, NULL);
				if ( n < 0) {
					perror("recvfrom"); goto out;
				}

				off = msg_decode(packet, (size_t)n, &msg_header);

				if (off < 0 || msg_header.type != MSG_TYPE_DATA) {
					printf("[%s][drop] bad header(%d bytes)\n", ifname, n);
					continue;
				}

				if  (data_decode(packet + off, (size_t)(n - off), &data_header) < 0) {
					printf("[%s][drop] short data header\n", ifname);
				}

				off += sizeof(data_header_t);

				n -= off;

				if (n < IP_MIN_HDR || IP_VERSION(packet) != 4) {
					printf("[%s][drop] udp: not IPv4 (%d bytes)\n", ifname, n);
					continue;
				}

				printf("[%s][udp->tun] %d bytes, proto=%d, icmp type=%d\n", ifname, n, packet[IP_PROTO_OFF], packet[IP_IHL(packet)]);

				if (write(tun_fd, packet, n) < 0) {
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

