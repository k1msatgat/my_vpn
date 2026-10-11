#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
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
#include "auth.h"
#include "device.h"

#define MAX_EVENTS 8

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* 벽시계(time)는 관리자가 시간을 고치거나 NTP 가 맞추면 뛴다. nonce 의 수명처럼
 * "몇 초가 지났는가"만 필요한 곳에는 뒤로 가지 않는 시계를 쓴다. */
static time_t mono_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec;
}

/* 거절하고 이유를 알려 준다. 응답이 없는 것과 거절된 것을 클라이언트가 구분해야
 * 사용자에게 무엇을 고치라고 말할 수 있다. */
static void send_deny(int32_t sock, const struct sockaddr_in *dst, socklen_t dstlen,
		uint32_t session_idx, uint8_t reason)
{
	uint8_t res[sizeof(msg_header_t) + 1];
	msg_header_t msg_header;
	int32_t off;

	memset(&msg_header, 0, sizeof(msg_header));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_DENY;
	msg_header.session_idx = session_idx;

	off = msg_encode(res, sizeof(res), &msg_header);
	res[off++] = reason;

	if (sendto(sock, res, off, 0, (const struct sockaddr *)dst, dstlen) < 0) {
		perror("sendto deny");
	}
}

/* REQ_HANDSHAKE: tun_ip(4) | device_id(16)
 *
 * 여기서는 아무것도 믿지 않는다. ID 는 공개된 이름표라 누구나 댈 수 있다. 그래서
 * 세션을 주지 않고 "그 기기라면 이 nonce 에 서명해 봐라"만 돌려준다. */
static void handle_request(int32_t sock, const uint8_t *packet, int32_t n,
		const struct sockaddr_in *src, socklen_t srclen)
{
	char ids[DEVICE_ID_STRLEN];
	uint8_t res[sizeof(msg_header_t) + AUTH_NONCE_LEN];
	const uint8_t *body = packet + sizeof(msg_header_t);
	msg_header_t msg_header;
	device_t *dev;
	int32_t off;

	if (n < (int32_t)(sizeof(msg_header_t) + REQ_HS_PAYLOAD_LEN)) {
		printf("[HS] short request (%d bytes)\n", n);
		return;
	}

	dev = device_find(body + IP_ADDR_LEN);
	if (dev == NULL) {
		printf("[HS] unknown device %s\n", device_id_str(body + IP_ADDR_LEN, ids));
		send_deny(sock, src, srclen, 0, DENY_UNKNOWN_DEVICE);
		return;
	}

	/* nonce 는 기기마다 하나만 기억한다. 새 요청이 오면 이전 것은 덮어써진다. */
	if (auth_random(dev->nonce, sizeof(dev->nonce)) < 0) {
		perror("getrandom");
		return;
	}
	dev->nonce_at = mono_now();
	dev->nonce_valid = 1;

	memset(&msg_header, 0, sizeof(msg_header));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_CHALLENGE;

	off = msg_encode(res, sizeof(res), &msg_header);
	memcpy(res + off, dev->nonce, AUTH_NONCE_LEN);
	off += AUTH_NONCE_LEN;

	if (sendto(sock, res, off, 0, (const struct sockaddr *)src, srclen) < 0) {
		perror("sendto challenge");
	}
}

/* AUTH: tun_ip(4) | device_id(16) | nonce(16) | posture(8) | mac(32)
 *
 * 검사 순서에 뜻이 있다. 키를 가졌다는 것(mac)을 확인하기 전에는 "터널 IP 가
 * 틀렸다" 같은 구체적인 이유를 알려 주지 않는다. 인증되지 않은 상대에게 설정
 * 내용을 흘리지 않기 위해서다. */
static void handle_auth(int32_t sock, const uint8_t *packet, int32_t n,
		const struct sockaddr_in *src, socklen_t srclen)
{
	char ids[DEVICE_ID_STRLEN];
	char ib[INET_ADDRSTRLEN];
	uint8_t res[sizeof(msg_header_t)];
	uint8_t mac[AUTH_MAC_LEN];
	const uint8_t *body = packet + sizeof(msg_header_t);
	const uint8_t *nonce = body + IP_ADDR_LEN + DEVICE_ID_LEN;
	msg_header_t msg_header;
	struct in_addr tun_ip;
	device_t *dev;
	peer_t *peer;
	int fresh;

	if (n < (int32_t)(sizeof(msg_header_t) + AUTH_PAYLOAD_LEN)) {
		printf("[HS] short auth (%d bytes)\n", n);
		return;
	}

	memcpy(&tun_ip, body, IP_ADDR_LEN);

	dev = device_find(body + IP_ADDR_LEN);
	if (dev == NULL) {
		printf("[HS] unknown device %s\n", device_id_str(body + IP_ADDR_LEN, ids));
		send_deny(sock, src, srclen, 0, DENY_UNKNOWN_DEVICE);
		return;
	}
	device_id_str(dev->id, ids);

	/* nonce 는 한 번만 쓴다. 맞든 틀리든 AUTH 를 받은 순간 버린다. 남겨 두면 누군가
	 * 엿들은 AUTH 를 그대로 다시 보내 세션을 또 열 수 있다. */
	fresh = dev->nonce_valid &&
		mono_now() - dev->nonce_at <= NONCE_TTL_SEC &&
		memcmp(nonce, dev->nonce, AUTH_NONCE_LEN) == 0;
	dev->nonce_valid = 0;

	if (!fresh ||
			auth_hmac(dev->key, body, AUTH_SIGNED_LEN, mac) < 0 ||
			!auth_mac_equal(mac, body + AUTH_SIGNED_LEN)) {
		printf("[HS] bad auth from device %s\n", ids);
		send_deny(sock, src, srclen, 0, DENY_BAD_AUTH);
		return;
	}

	/* 여기부터는 그 기기가 맞다. */
	if (tun_ip.s_addr != dev->tun_ip.s_addr) {
		printf("[HS] device %s asked for %s, not its tunnel ip\n", ids, ip_str(tun_ip, ib));
		send_deny(sock, src, srclen, 0, DENY_TUN_IP);
		return;
	}

	peer = peer_register(tun_ip, src, srclen);
	if (peer == NULL) {
		printf("[HS] register failed for %s\n", ip_str(tun_ip, ib));
		return;
	}

	printf("[HS] device %s authenticated as %s\n", ids, ip_str(tun_ip, ib));

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
	int nd;
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

	/* 서버를 띄우지 않고 암호 도구만 확인한다. Windows 클라이언트의
	 * `vpn_client selftest` 와 같은 벡터를 쓴다. */
	if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
		return auth_selftest();
	}

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s <port> <device-file> [acl-file]\n", argv[0]);
		fprintf(stderr, "       %s selftest\n", argv[0]);
		return 1;
	}
	port = strtol(argv[1], &end, 10);
	if (*end != '\0' || port <= 0 || port > 65535) {
		fprintf(stderr, "invalid port : %s\n", argv[1]);
		return 1;
	}

	peer_init();
	acl_init();
	device_init();

	/* 기기 목록은 ACL 과 달리 생략할 수 없다. 없으면 누구도 받아 줄 수 없고,
	 * "목록이 없으면 모두 허용"으로 두면 설정 실수가 곧 무인증 서버가 된다. */
	nd = device_load(argv[2]);
	if (nd < 0) {
		fprintf(stderr, "device_load failed: %s\n", argv[2]);
		return 1;
	}
	printf("device: %d devices loaded from %s\n", nd, argv[2]);
	device_dump();

	if (argc == 4) {
		int nr = acl_load(argv[3]);
		if (nr < 0) {
			fprintf(stderr, "acl_load failed: %s\n", argv[3]);
			return 1;
		}
		printf("acl: %d rules loaded from %s\n", nr, argv[3]);
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
		nev = epoll_wait(epfd, events, MAX_EVENTS, 1000);
		if (nev < 0) {
			if (errno == EINTR){
				continue;
			}
			perror("epoll_wait");
			break;
		}

		peer_check_expired();

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
					handle_request(sock, packet, n, &src, srclen);
					continue;
				}
				if (msg_header.type == MSG_TYPE_AUTH) {
					handle_auth(sock, packet, n, &src, srclen);
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

