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
#include "crypt.h"
#include "secure.h"
#include "device.h"
#include "status.h"

#define MAX_EVENTS 8

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* 시그널 핸들러 안에서는 파일을 읽을 수 없다 (fopen / printf 는 핸들러에서 불러도
 * 안전한 함수가 아니다). 표시만 해 두고 메인 루프가 읽는다. */
static volatile sig_atomic_t reload_requested = 0;

static void on_reload(int sig)
{
	(void)sig;
	reload_requested = 1;
}

/* 핸드셰이크를 거절하고 이유를 알려 준다. 응답이 없는 것과 거절된 것을 클라이언트가
 * 구분해야 사용자에게 무엇을 고치라고 말할 수 있다.
 *
 * DENY: reason(1) | mac(32)
 *
 * dev 와 auth_mac 을 주면 그 기기의 키로 mac 을 만든다. 서명이 맞는 AUTH 를 받은 뒤의
 * 거절에만 줄 수 있다. 그 전에는 상대가 그 기기인지 모르고, 틀린 키로 온 상대에게는
 * 우리가 만든 mac 이 어차피 맞지 않는다. 그때는 0 으로 채워 보낸다 — 클라이언트는 mac 이
 * 맞는 거절만 서버의 답으로 믿고, 맞지 않는 것은 다른 답이 오지 않을 때에만 받아들인다. */
static void send_deny(int32_t sock, const struct sockaddr_in *dst, socklen_t dstlen,
		uint8_t reason, const device_t *dev, const uint8_t *auth_mac)
{
	uint8_t res[DENY_HS_LEN];
	msg_header_t msg_header;
	int32_t off;

	memset(res, 0, sizeof(res));
	memset(&msg_header, 0, sizeof(msg_header));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_DENY;

	off = msg_encode(res, sizeof(res), &msg_header);
	res[off++] = reason;

	if (dev != NULL && auth_mac != NULL &&
			secure_deny_mac(dev->key, reason, auth_mac, res + off) < 0) {
		return;
	}

	if (reason <= DENY_POSTURE) {
		stats.deny[reason]++;
	}

	if (sendto(sock, res, sizeof(res), 0, (const struct sockaddr *)dst, dstlen) < 0) {
		perror("sendto deny");
	}
}

/* 열려 있는 세션을 거절한다. 세션 키로 감싸므로 서버만 만들 수 있다 — 세션 번호를
 * 엿본 제3자가 가짜 거절로 남의 접속을 끊을 수 없다. 보낸 뒤 세션을 내리는 것은
 * 부르는 쪽의 일이다. */
static void send_session_deny(int32_t sock, peer_t *peer, const struct sockaddr_in *dst,
		socklen_t dstlen, uint8_t reason)
{
	uint8_t res[PKT_OVERHEAD + 1];
	int n;

	res[PKT_HDR_LEN] = reason;
	n = secure_seal(&peer->secure, MSG_TYPE_DENY, peer->session_idx, res, sizeof(res), 1);
	if (n < 0) {
		return;
	}

	if (reason <= DENY_POSTURE) {
		stats.deny[reason]++;
	}

	if (sendto(sock, res, n, 0, (const struct sockaddr *)dst, dstlen) < 0) {
		perror("sendto deny");
	}
}

/* REQ_HANDSHAKE: tun_ip(4) | device_id(16)
 *
 * 여기서는 아무것도 믿지 않는다. ID 는 공개된 이름표라 누구나 댈 수 있다. 그래서
 * 세션을 주지 않고 "그 기기라면 이 nonce 에 서명해 봐라"만 돌려준다. 키를 만들고
 * 합의하는 비싼 계산도 여기서는 하지 않는다 — 출발지를 위조한 REQ 를 쏟아부어 서버를
 * 바쁘게 만들 수 없다. */
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
		send_deny(sock, src, srclen, DENY_UNKNOWN_DEVICE, NULL, NULL);
		return;
	}

	/* nonce 는 기기마다 하나만 기억한다. 아직 살아 있는 것이 있으면 새로 뽑지 않고
	 * 그것을 다시 내준다. 요청마다 덮어쓰면, 기기 ID(공개된 이름표)를 아는 누구나
	 * REQ 를 계속 보내는 것만으로 진짜 기기가 서명 중인 nonce 를 무효로 만들 수 있다.
	 * nonce 는 비밀이 아니다 — 남이 받아 가도 키가 없으면 서명하지 못한다. */
	if (!dev->nonce_valid || mono_now() - dev->nonce_at > NONCE_TTL_SEC) {
		if (crypt_random(dev->nonce, sizeof(dev->nonce)) < 0) {
			perror("getrandom");
			return;
		}
		dev->nonce_at = mono_now();
		dev->nonce_valid = 1;
	}

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

/* 인증이 끝난 기기와 세션 키를 합의하고 RES_HANDSHAKE 를 만든다.
 *
 *   RES_HANDSHAKE: pub_s(64) | tag(16)
 *
 * 일회용 키 쌍을 새로 만들어 클라이언트의 것과 합의하고, 거기에 기기 키와 주고받은
 * 패킷의 해시를 섞어 세션 키를 뽑는다 (secure.h). tag 는 그 키로 만든다 — 클라이언트는
 * tag 가 맞는 것을 보고 상대가 이 기기의 키를 아는 서버임을 안다.
 *
 * res 에 보낼 패킷을 채우고 길이를 돌려준다. 실패하면 -1. */
static int open_session(peer_t *peer, const device_t *dev, const uint8_t *auth, uint8_t *res, size_t cap)
{
	uint8_t secret[VPN_SECRET_LEN];
	secure_keys_t keys;
	msg_header_t msg_header;
	crypt_ecdh_t *ecdh;
	int len = -1;

	memset(&msg_header, 0, sizeof(msg_header));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_RES_HANDSHAKE;
	msg_header.session_idx = peer->session_idx;
	msg_encode(res, cap, &msg_header);

	ecdh = crypt_ecdh_new(res + sizeof(msg_header_t));
	if (ecdh == NULL) {
		return -1;
	}

	/* 클라이언트의 공개키가 곡선 위의 점이 아니면 여기서 실패한다. */
	if (crypt_ecdh_derive(ecdh, auth + sizeof(msg_header_t) + AUTH_OFF_PUB, secret) < 0 ||
			secure_derive(dev->key, secret, auth, res, &keys) < 0) {
		goto out;
	}

	/* 서버는 S->C 키로 보내고 C->S 키로 받는다. */
	if (secure_session_open(&peer->secure, keys.s2c, keys.c2s) < 0) {
		goto out;
	}

	len = secure_res_seal(keys.res, res, cap, 0);

out:
	crypt_ecdh_free(ecdh);
	crypt_wipe(secret, sizeof(secret));
	crypt_wipe(&keys, sizeof(keys));

	return len;
}

/* AUTH: tun_ip(4) | device_id(16) | nonce(16) | pub_c(64) | posture(8) | mac(32)
 *
 * 검사 순서에 뜻이 있다. 키를 가졌다는 것(mac)을 확인하기 전에는 "터널 IP 가
 * 틀렸다" 같은 구체적인 이유를 알려 주지 않는다. 인증되지 않은 상대에게 설정
 * 내용을 흘리지 않기 위해서다. 키 합의도 그 뒤에 한다. */
static void handle_auth(int32_t sock, const uint8_t *packet, int32_t n,
		const struct sockaddr_in *src, socklen_t srclen)
{
	char ids[DEVICE_ID_STRLEN];
	char ib[INET_ADDRSTRLEN];
	uint8_t res[RES_HS_MIN_LEN];
	uint8_t mac[AUTH_MAC_LEN];
	const uint8_t *body = packet + sizeof(msg_header_t);
	const uint8_t *auth_mac = body + AUTH_OFF_MAC;
	posture_report_t posture;
	struct in_addr tun_ip;
	device_t *dev;
	peer_t *peer;
	int fresh;
	int len;

	if (n < (int32_t)AUTH_PACKET_LEN) {
		printf("[HS] short auth (%d bytes)\n", n);
		return;
	}

	memcpy(&tun_ip, body + AUTH_OFF_TUN_IP, IP_ADDR_LEN);

	dev = device_find(body + AUTH_OFF_DEVICE_ID);
	if (dev == NULL) {
		printf("[HS] unknown device %s\n", device_id_str(body + AUTH_OFF_DEVICE_ID, ids));
		send_deny(sock, src, srclen, DENY_UNKNOWN_DEVICE, NULL, NULL);
		return;
	}
	device_id_str(dev->id, ids);

	fresh = dev->nonce_valid &&
		mono_now() - dev->nonce_at <= NONCE_TTL_SEC &&
		memcmp(body + AUTH_OFF_NONCE, dev->nonce, AUTH_NONCE_LEN) == 0;

	/* 틀린 AUTH 는 nonce 를 건드리지 않는다. 받기만 하면 버리던 때에는 아무 값이나
	 * 채운 AUTH 한 통으로 진짜 기기의 핸드셰이크를 깨뜨릴 수 있었다. 그 기기가 보냈다고
	 * 확인되지 않은 패킷은 그 기기의 상태를 바꾸지 못한다.
	 *
	 * mac 은 헤더부터 덮는다. 일회용 공개키도 그 안이라, 사이에 낀 쪽이 자기 공개키로
	 * 바꿔 끼우면 여기서 걸린다. */
	if (!fresh ||
			crypt_hmac(dev->key, AUTH_KEY_LEN, packet, AUTH_SIGNED_LEN, mac) < 0 ||
			!crypt_equal(mac, auth_mac, AUTH_MAC_LEN)) {
		printf("[HS] bad auth from device %s\n", ids);
		send_deny(sock, src, srclen, DENY_BAD_AUTH, NULL, NULL);
		return;
	}

	/* 여기부터는 그 기기가 맞다. nonce 는 한 번만 쓴다 — 서명이 맞은 AUTH 를 받은
	 * 순간, 뒤의 검사에서 거절되더라도 버린다. 남겨 두면 엿들은 AUTH 를 그대로 다시
	 * 보내 세션을 또 열 수 있다. */
	dev->nonce_valid = 0;

	if (tun_ip.s_addr != dev->tun_ip.s_addr) {
		printf("[HS] device %s asked for %s, not its tunnel ip\n", ids, ip_str(tun_ip, ib));
		send_deny(sock, src, srclen, DENY_TUN_IP, dev, auth_mac);
		return;
	}

	/* 누구인지는 확인됐다. 이제 그 기기가 지금 믿을 만한 상태인지 본다. 포스처는
	 * mac 의 서명 범위 안이라 전달 도중에 고쳐지지 않았다. */
	posture_decode(body + AUTH_OFF_POSTURE, sizeof(posture), &posture);
	if (!device_posture_ok(dev, &posture)) {
		printf("[HS] device %s posture rejected (build=%u firewall=%u antivirus=%u)\n",
				ids, posture.os_build, posture.firewall, posture.antivirus);
		send_deny(sock, src, srclen, DENY_POSTURE, dev, auth_mac);
		return;
	}

	/* 새로 인증하면 새 세션이다. 번호도, 키도, counter 도 새로 시작한다. 예전 세션에서
	 * 엿들은 패킷은 번호가 달라 찾지 못하고, 찾더라도 키가 달라 열리지 않는다. */
	peer = peer_lookup(tun_ip);
	if (peer != NULL) {
		peer_drop(peer);
	}

	peer = peer_register(tun_ip, src, srclen);
	if (peer == NULL) {
		printf("[HS] register failed for %s\n", ip_str(tun_ip, ib));
		return;
	}

	len = open_session(peer, dev, packet, res, sizeof(res));
	if (len < 0) {
		printf("[HS] key agreement failed for device %s\n", ids);
		peer_drop(peer);
		return;
	}

	memcpy(peer->device_id, dev->id, DEVICE_ID_LEN);
	peer->posture = posture;
	stats.handshake_ok++;

	printf("[HS] device %s authenticated as %s (build=%u firewall=%u antivirus=%u)\n",
			ids, ip_str(tun_ip, ib), posture.os_build, posture.firewall, posture.antivirus);

	if (sendto(sock, res, len, 0, (const struct sockaddr *)&peer->outer, peer->outer_len) < 0){
		perror("sendto handshake");
	}
}

/* KEEPALIVE (S -> C): 암호문 없이 tag 만. 클라이언트는 이것으로 서버가 살아 있고 세션이
 * 그대로임을 안다. tag 가 붙으므로 서버인 척하는 쪽이 대신 보내 줄 수 없다. */
static void send_keepalive(int32_t sock, peer_t *peer)
{
	uint8_t packet[PKT_OVERHEAD];
	int n;

	n = secure_seal(&peer->secure, MSG_TYPE_KEEPALIVE, peer->session_idx, packet, sizeof(packet), 0);
	if (n < 0) {
		return;
	}

	if (sendto(sock, packet, n, 0, (const struct sockaddr *)&peer->outer, peer->outer_len) < 0) {
		perror("sendto");
	}
}

/* KEEPALIVE (C -> S). 세션 키로 푼 뒤의 내용은 posture(8) 이다.
 *
 * 핸드셰이크에서 한 번 통과했다고 세션이 끝날 때까지 믿지 않는다. 클라이언트는
 * keepalive 마다 지금의 포스처를 다시 싣고, 서버는 받을 때마다 정책에 다시 대 본다.
 *
 * 여기까지 온 패킷은 tag 가 맞았으므로 그 기기가 이 세션에 보낸 것이다. 위조된 것과
 * 다시 보낸 것은 이미 걸러졌고, 그런 패킷으로는 세션이 끊기지 않는다. 포스처가 정책에
 * 못 미치면 DENY 로 알리고 세션을 내린다 — 기기 스스로 "지금 방화벽이 꺼져 있다"고
 * 말한 것이다. */
static void handle_keepalive(int32_t sock, peer_t *peer, const uint8_t *body, int32_t n,
		uint64_t counter, const struct sockaddr_in *src, socklen_t srclen)
{
	char ids[DEVICE_ID_STRLEN];
	posture_report_t posture;
	device_t *dev;

	if (n < (int32_t)sizeof(posture)) {
		printf("[drop] keepalive without posture (%d bytes)\n", n);
		stats.keepalive_dropped++;
		return;
	}

	device_id_str(peer->device_id, ids);

	/* 세션이 열린 뒤 기기가 목록에서 지워졌다. */
	dev = device_find(peer->device_id);
	if (dev == NULL) {
		printf("[ka] device %s is no longer registered\n", ids);
		send_session_deny(sock, peer, src, srclen, DENY_UNKNOWN_DEVICE);
		peer_drop(peer);
		return;
	}

	/* 포스처 보고는 순서대로만 받는다. 데이터 패킷은 순서가 바뀌어 와도 받지만
	 * (secure.c 의 창), 보고는 "지금의 상태"라 늦게 온 예전 것이 최신 것을 덮으면 안
	 * 된다. 방화벽이 켜져 있던 때의 보고를 길목에서 붙잡아 두었다가, 끈 뒤에 풀어
	 * 놓는 것을 막는다. */
	if (peer->ka_seen && counter <= peer->ka_counter) {
		printf("[drop] keepalive out of order (counter %llu) idx=%08x\n",
				(unsigned long long)counter, peer->session_idx);
		stats.keepalive_dropped++;
		return;
	}
	peer->ka_counter = counter;
	peer->ka_seen = 1;

	posture_decode(body, sizeof(posture), &posture);
	if (!device_posture_ok(dev, &posture)) {
		printf("[ka] device %s posture rejected (build=%u firewall=%u antivirus=%u)\n",
				ids, posture.os_build, posture.firewall, posture.antivirus);
		send_session_deny(sock, peer, src, srclen, DENY_POSTURE);
		peer_drop(peer);
		return;
	}

	/* 기기 정책은 통과했다. 보고된 값은 세션에 남겨 둔다 — 기기 정책보다 높은 수준을
	 * 요구하는 ACL 규칙이 있으면 다음 패킷부터 이 값으로 판정한다. */
	peer->posture = posture;

	peer_touch(peer, src, srclen);
	send_keepalive(sock, peer);
}

int main(int argc, char *argv[])
{
	char ifname[IFNAMSIZ];
	char ib[INET_ADDRSTRLEN], ob[INET_ADDRSTRLEN];
	int tun_fd, sock, n, sent, port;
	int epfd = -1, nev, i;
	int acl_enabled = 0;
	int verdict;
	int opt;
	int status_fd = -1;
	const char *status_path = NULL;
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
	uint64_t counter;
	int32_t off;
	char *end;
	int ret = 1;
	int fd = 0;

	/* 서버를 띄우지 않고 암호 도구만 확인한다. Windows 클라이언트의
	 * `vpn_client selftest` 와 같은 벡터를 쓴다. */
	if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
		return crypt_selftest();
	}

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		if (opt != 's') {
			argc = 0;
			break;
		}
		status_path = optarg;
	}

	/* 옵션을 걷어내고 나머지 인자가 다시 argv[1] 부터 오게 한다. */
	if (argc != 0) {
		argv[optind - 1] = argv[0];
		argv += optind - 1;
		argc -= optind - 1;
	}

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s [-s status-socket] <port> <device-file> [acl-file]\n", argv[0]);
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
	signal(SIGHUP, on_reload);

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

	if (status_path != NULL) {
		status_fd = status_listen(status_path);
		if (status_fd < 0) {
			goto out;
		}

		ev.events  = EPOLLIN;
		ev.data.fd = status_fd;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, status_fd, &ev) < 0) {
			perror("epoll_ctl: status_fd"); goto out;
		}
		printf("status socket: %s\n", status_path);
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

		/* kill -HUP 으로 기기 목록과 ACL 을 다시 읽는다. 서버를 내렸다 올리면 모든
		 * 세션이 끊기지만, 이렇게 하면 정책을 바꾸거나 기기 하나를 지우는 동안
		 * 나머지는 그대로 붙어 있다. 열려 있는 세션은 건드리지 않는다 — 다음
		 * keepalive 가 새 목록에 대 보고 걸러진다. epoll_wait 의 타임아웃이 1초라
		 * 시그널을 받고 늦어도 1초 안에 여기에 온다. */
		if (reload_requested) {
			reload_requested = 0;

			/* ACL 은 패킷마다 대 보므로 다음 패킷부터 바로 새 규칙이다. 시작할 때
			 * 규칙 파일을 주지 않은 서버는 다시 읽어도 ACL 이 켜지지 않는다. 파일이
			 * 생겼다고 도중에 켜면, 파일이 사라졌을 때 끄는 것도 받아 줘야 하고 그건
			 * 설정 실수 하나로 전부 열리는 길이다. */
			if (acl_enabled) {
				int nr = acl_load(argv[3]);
				if (nr < 0) {
					printf("acl: reload failed, keeping the previous rules\n");
				}
				else {
					printf("acl: reloaded, %d rules\n", nr);
					acl_dump();
				}
			}

			nd = device_load(argv[2]);
			if (nd < 0) {
				printf("device: reload failed, keeping the previous list\n");
			}
			else {
				printf("device: reloaded, %d devices\n", nd);
				device_dump();
			}
		}

		for (i = 0; i < nev; i++) {
			fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fprintf(stderr, "[err] fd=%d events=0x%x\n",
						fd, events[i].events);
				continue;
			}

			if (fd == status_fd) {
				status_serve(status_fd, acl_enabled);
			}
			else if (fd == sock) {
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
					stats.unknown_session++;
					continue;
				}

				/* 세션 번호는 와이어에 그대로 보인다. 그 번호를 댔다고 그 기기인 것은
				 * 아니다. 세션 키로 열리는 패킷만 그 기기가 보낸 것이고, 열리지 않는
				 * 패킷은 세션의 어떤 상태도 바꾸지 못한 채 여기서 버려진다. */
				n = secure_open(&peer->secure, packet, (size_t)n, &counter);
				if (n == SECURE_REPLAY) {
					printf("[drop] counter already seen idx=%08x\n", peer->session_idx);
					stats.replayed++;
					continue;
				}
				if (n < 0) {
					printf("[drop] tag does not match idx=%08x\n", peer->session_idx);
					stats.bad_tag++;
					continue;
				}

				ip = packet + PKT_HDR_LEN;

				switch (msg_header.type) {
					case MSG_TYPE_DATA:
						break;
					case MSG_TYPE_KEEPALIVE:
						handle_keepalive(sock, peer, ip, n, counter, &src, srclen);
						continue;
					default:
						printf("[drop] unknown type %u\n", msg_header.type);
						continue;
				}

				if (n < IP_MIN_HDR || IP_VERSION(ip) != 4){
					continue;
				}

				memcpy(&inner, ip + IP_SRC_OFF, IP_ADDR_LEN);

				if (inner.s_addr != peer->inner.s_addr) {
					printf("[drop] spoofed src %s for idx %08x\n", ip_str(inner, ib), peer->session_idx);
					stats.spoofed_src++;
					continue;
				}

				verdict = acl_enabled ? acl_check(ip, n, &peer->posture) : ACL_ALLOW;
				if (verdict != ACL_ALLOW) {
					memcpy(&inner_dst, ip + IP_DST_OFF, IP_ADDR_LEN);
					printf("[deny] %s -> %s proto=%d (%d bytes)",
							ip_str(inner, ib), ip_str(inner_dst, ob),
							ip[IP_PROTO_OFF], n);
					stats.acl_denied++;
					peer->acl_denied++;
					if (verdict == ACL_DENY_POSTURE) {
						stats.acl_denied_posture++;
						printf(" posture below the rule (build=%u firewall=%u antivirus=%u)",
								peer->posture.os_build, peer->posture.firewall,
								peer->posture.antivirus);
					}
					printf("\n");
					continue;
				}

				peer_touch(peer, &src, srclen);
				peer->rx_packets++;
				peer->rx_bytes += (uint64_t)n;

				printf("[udp->tun] %d bytes from %s:%d (inner src %s), icmp type=%d\n",
						n, ip_str(src.sin_addr, ob), ntohs(src.sin_port),
						ip_str(inner, ib), ip[IP_IHL(ip)]);

				if (write(tun_fd, ip, n) < 0) {
					perror("write tun");
				}
			}
			else if (fd == tun_fd) {
				/* 앞에는 헤더가, 뒤에는 tag 가 붙을 자리를 남기고 읽는다. 암호화는
				 * 읽은 자리에서 하므로 패킷을 옮기지 않는다. */
				n = read(tun_fd, packet + PKT_HDR_LEN, sizeof(packet) - PKT_OVERHEAD);
				if (n < 0) { perror("read tun"); goto out; }

				ip = packet + PKT_HDR_LEN;
				if (n < IP_MIN_HDR || IP_VERSION(ip) != 4) {
					printf("[skip] non-IPv4 (ver=%d, %d bytes)\n",
							IP_VERSION(ip), n);
					continue;
				}

				memcpy(&inner, ip + IP_DST_OFF, IP_ADDR_LEN);

				peer = peer_lookup(inner);
				if (peer == NULL) {
					printf("[drop] no peer for %s\n", ip_str(inner, ib));
					continue;
				}

				printf("[tun->udp] %d bytes to %s (%s:%d), icmp type=%d\n",
						n, ip_str(inner, ib),
						ip_str(peer->outer.sin_addr, ob), ntohs(peer->outer.sin_port),
						ip[IP_IHL(ip)]);

				peer->tx_packets++;
				peer->tx_bytes += (uint64_t)n;

				n = secure_seal(&peer->secure, MSG_TYPE_DATA, peer->session_idx,
						packet, sizeof(packet), (size_t)n);
				if (n < 0) {
					printf("[drop] seal failed for %s\n", ip_str(inner, ib));
					continue;
				}

				sent = sendto(sock, packet, n, 0,
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
	if (status_fd >= 0) {
		close(status_fd);
		unlink(status_path);
	}
	close(sock);
	close(tun_fd);
	return ret;
}

