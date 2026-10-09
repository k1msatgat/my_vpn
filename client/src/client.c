#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <net/if.h>

#include "common.h"

#define MAX_EVENTS 8

#define HS_TIMEOUT_SEC 1
#define HS_RETRY 5

#define KEEPALIVE_INTERVAL_MS 10000

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

static uint64_t get_current_time_ms(void){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)(ts.tv_nsec / 1000000));
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

	for (tries = 1; tries <= HS_RETRY && running; tries++) {
		printf("[HS] request %d/%d\n", tries, HS_RETRY);
		if (sendto(sock, req, off, 0, (struct sockaddr*)&session->server, sizeof(session->server)) < 0){
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

static void send_keepalive(int32_t sock, const session_t *session) {
	msg_header_t msg_header;
	int32_t off;
	int32_t sent;
	uint8_t packet[sizeof(msg_header)];

	memset(&msg_header,0, sizeof(msg_header_t));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_KEEPALIVE;
	msg_header.session_idx = session->idx;
	off = msg_encode(packet, sizeof(packet), &msg_header);

	sent = sendto(sock, packet, off, 0, (struct sockaddr *)&session->server, sizeof(session->server));
	if (sent < 0) {
		perror("sendto");
	}
}

int main(int argc, char *argv[])
{
	char ifname[IFNAMSIZ];
	int tun_fd, sock, n, sent;
	int epfd = -1, nev, i;
	struct epoll_event ev;
	struct epoll_event events[MAX_EVENTS];
	int32_t fd = 0;
	struct sockaddr_in from;
	socklen_t fromlen;

	uint8_t packet[BUF_SIZE];
	uint8_t *ip;
	msg_header_t msg_header;
	data_header_t data_header;
	int off = 0;
	char *end;
	int32_t server_port;
	int ret = 1;
	uint64_t last_tx_ms = 0;

	session_t session;

	if (argc != 5){
		fprintf(stderr, "usage: %s <ifname> <server-ip> <port> <tunnel-ip>\n", argv[0]);
		fprintf(stderr, " e.g. %s tun0 49.247.139.39 9000 10.0.0.1\n", argv[0]);
		return 1;
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

	printf("[%s] tunneling to %s:%s\n", ifname, argv[2], argv[3]);

	memset(&session, 0, sizeof(session_t));

	session.server.sin_family = AF_INET;

	server_port = strtol(argv[3], &end, 10);
	if (*end != '\0' || server_port <= 0 || server_port > 65535) {
		fprintf(stderr, "invalid port : %s\n", argv[3]);
		goto out;
	}
	session.server.sin_port = htons((uint16_t)server_port);

	if (inet_pton(AF_INET, argv[2], &session.server.sin_addr) != 1) {
		fprintf(stderr, "invalid server ip : %s\n", argv[2]);
		goto out;
	}

	if (inet_pton(AF_INET, argv[4], &session.tun_ip) != 1){
		fprintf(stderr, "invalid tun ip : %s\n", argv[4]);
		goto out;
	}

	if (do_handshake(sock, &session)) {
		goto out;
	}

	last_tx_ms = get_current_time_ms();

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		perror("epoll_create1");
		goto out;
	}

	ev.events = EPOLLIN;
	ev.data.fd = tun_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, tun_fd, &ev) < 0){
		perror("epoll_ctl: tun_fd");
		goto out;
	}

	ev.events = EPOLLIN;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0){
		perror("epoll_ctl: sock");
		goto out;
	}

	printf("[%s] epoll ready (epfd=%d, tun_fd=%d, sock=%d)\n", ifname, epfd, tun_fd, sock);

	while (running){
		nev = epoll_wait(epfd, events, MAX_EVENTS, 1000);
		if (nev < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("epoll_wait");
			break;
		}

		if (last_tx_ms + KEEPALIVE_INTERVAL_MS < get_current_time_ms()){
			send_keepalive(sock, &session);
			last_tx_ms = get_current_time_ms();
		}

		for (i=0;i<nev;i++){
			fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fprintf(stderr, "[%s][err] fd=%d events=0x%x\n", ifname, fd, events[i].events);
				continue;
			}

			if (fd == tun_fd) {
				n = read(tun_fd, packet + PKT_HDR_LEN, sizeof(packet) - PKT_HDR_LEN);
				if (n < 0) {
					perror("read tun");
					goto out;
				}

				ip = packet + PKT_HDR_LEN;
				if (n < IP_MIN_HDR || IP_VERSION(ip) != 4) {
					printf("[%s][skip] non-IPv4 (ver=%d, %d bytes)\n", ifname, IP_VERSION(ip), n);
				}
				else {
					printf("[%s][tun->udp] %d bytes, proto=%d, icmp type=%d\n", ifname, n, ip[IP_PROTO_OFF], ip[IP_IHL(ip)]);
					memset(&msg_header,0, sizeof(msg_header_t));
					msg_header.version = PROTOCOL_VERSION;
					msg_header.type = MSG_TYPE_DATA;
					msg_header.session_idx = session.idx;
					data_header.counter = session.tx_counter++;

					off = msg_encode(packet, sizeof(packet), &msg_header);
					off += data_encode(packet+off, sizeof(packet) - off, &data_header);

					sent = sendto(sock, packet, off + n, 0, (struct sockaddr *)&session.server, sizeof(session.server));
					if (sent < 0) {
						perror("sendto");
					}
					last_tx_ms = get_current_time_ms();
				}

			}
			else if(fd == sock) {
				fromlen = sizeof(from);
				n = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *)&from, &fromlen);
				if ( n < 0) {
					perror("recvfrom"); goto out;
				}

				if (from.sin_addr.s_addr != session.server.sin_addr.s_addr ||
						from.sin_port != session.server.sin_port) {
					printf("[%s][drop] not from server\n", ifname);
					continue;
				}

				off = msg_decode(packet, (size_t)n, &msg_header);

				if (msg_header.session_idx != session.idx) {
					printf("[%s][drop] wrong session [%08x][%08x]\n", ifname, msg_header.session_idx, session.idx);
					continue;
				}

				switch(msg_header.type){
					case MSG_TYPE_KEEPALIVE:
						continue; //우선 서버 장애 발생시는 고려하지 않음.
					case MSG_TYPE_DATA:
						break;
					default:
						printf("[%s][drop] bad header(%d bytes)\n", ifname, n);
						continue;
				}


				if (off < 0 || msg_header.version != PROTOCOL_VERSION || msg_header.type != MSG_TYPE_DATA) {
					continue;
				}

				if  (data_decode(packet + off, (size_t)(n - off), &data_header) < 0) {
					printf("[%s][drop] short data header\n", ifname);
					continue;
				}

				off += sizeof(data_header_t);

				n -= off;

				ip = packet + off;
				if (n < IP_MIN_HDR || IP_VERSION(ip) != 4) {
					printf("[%s][drop] udp: not IPv4 (%d bytes)\n", ifname, n);
					continue;
				}

				printf("[%s][udp->tun] %d bytes, proto=%d, icmp type=%d\n", ifname, n, ip[IP_PROTO_OFF], ip[IP_IHL(ip)]);

				if (write(tun_fd, ip, n) < 0) {
					perror("write tun");
				}
			}
		}
	}
	ret = 0;

out :
	if (epfd >= 0) {
		close(epfd);
	}
	close(sock);
	close(tun_fd);

	return ret;
}

