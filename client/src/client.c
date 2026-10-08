#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
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

#define HS_TIMEOUT_SEC 1
#define HS_RETRY 5

typedef struct session {
	uint32_t idx;
	uint64_t tx_counter;
	struct in_addr tun_ip;
	struct sockaddr_in server;
}session_t;

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int do_handshake(int32_t sock, session_t *session){
	uint8_t req[sizeof(msg_header_t) + sizeof(struct in_addr)];
	uint8_t res [BUF_SIZE];
	msg_header_t msg_header;
	struct sockaddr_in from;
	socklen_t fromlen;
	struct timeval tv;
	int32_t off = 0;
	int32_t n = 0;
	int32_t tries = 0;

	tv.tv_sec = HS_TIMEOUT_SEC;
	tv.tv_usec = 0;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
		perror("setsockopt SO_RCVTIMEO");
		return -1;
	}

	memset(&msg_header, 0, sizeof(msg_header_t));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_REQ_HANDSHAKE;
	msg_header.session_idx = 0;
	off = msg_encode(req, sizeof(req), &msg_header);
	memcpy(req + off, &session->tun_ip, sizeof(session->tun_ip));
	off += sizeof(session->tun_ip);

	for (tries = 0; tries < HS_RETRY; tries++) {
		printf("[HS] request %d/%d\n", tries, HS_RETRY);
		if (sendto(sock, req, off, 0, (struct sockaddr*)&session->server, sizeof(session->server))){
			perror("sendto handshake");
			return -1;
		}

		fromlen = sizeof(from);
		n = recvfrom(sock, res, sizeof(res), 0,  (struct sockaddr *)&from, &fromlen);
		if (n < 0){
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
				continue;
			}
			perror("recvfrom handshake");
			return -1;
		}

		if (from.sin_addr.s_addr != session->server.sin_addr.s_addr || from.sin_port != session->server.sin_port){
			continue;
		}

		if (msg_decode(res, (size_t)n, &msg_header) < 0){
				continue;
		}

		if (msg_header.version != PROTOCOL_VERSION || msg_header.type != MSG_TYPE_RES_HANDSHAKE || msg_header.session_idx == 0) {
			continue;
		}

		session->idx = msg_header.session_idx;

		tv.tv_sec = 0;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		printf("[HS] established idx=%08x\n", session->idx);
		return 0;
	}
	fprintf(stderr, "[HS] no response from server\n");
	return -1;
}

int main(int argc, char *argv[])
{
	char ifname[IFNAMSIZ];
	int tun_fd, sock, n, sent;
	int epfd, nev, i;
	struct epoll_event ev;
	struct epoll_event events[MAX_EVENTS];
	int32_t fd = 0;

	uint8_t packet[BUF_SIZE];
	msg_header_t msg_header;
	data_header_t data_header;
	int off = 0;

	session_t session;

	if (argc != 5){
		fprintf(stderr, "usage: %s <ifname> <server-ip> <port> <tunnel-ip>\n", argv[0]);
		fprintf(stderr, " e.g. %s tun0 49.247.139.39 9000 10.0.0.1\n", argv[0]);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	memset(&session, 0, sizeof(session_t));

	if (inet_pton(AF_INET, argv[4], &session.tun_ip) != 1){
		fprintf(stderr, "bad server address : %s\n", argv[4]);
		return 1;
	}

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

	printf("[%s] tunneling to %s:%s\n", ifname, argv[2], argv[3]);

	if (do_handshake(sock, &session)) {
		goto out;
	}

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
					msg_header.version = PROTOCOL_VERSION;
					msg_header.type = MSG_TYPE_DATA;
					msg_header.session_idx = session.idx;
					data_header.counter = session.tx_counter++;

					off = msg_encode(packet, sizeof(packet), &msg_header);
					off += data_encode(packet+off, sizeof(packet) - off, &data_header);

					sent = sendto(sock, packet, off, 0, (struct sockaddr *)&session.server, sizeof(session.server));
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

				if (msg_header.session_idx != session.idx) {
					printf("[%s][drop] wrong session [%08x][%08x]", ifname, msg_header.session_idx, session.idx);
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

