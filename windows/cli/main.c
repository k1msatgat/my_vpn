#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tunnel.h"
#include "posture.h"
#include "hmac.h"

#define TUN_PREFIX_LEN 24

static HANDLE stop_event;

static BOOL WINAPI on_console_ctrl(DWORD type)
{
	(void)type;
	SetEvent(stop_event);
	return TRUE;
}

static void on_log(void *ctx, const char *line)
{
	(void)ctx;
	printf("%s\n", line);
	fflush(stdout);
}

static const char *state_name(uint8_t state)
{
	switch (state) {
	case POSTURE_ON:	return "on";
	case POSTURE_OFF:	return "off";
	default:		return "unknown";
	}
}

/* 서버에 접속하지 않고 이 단말의 포스처만 읽어 찍는다. 어댑터를 만들지 않으므로
 * 관리자 권한이 필요 없다. */
static int print_posture(void)
{
	posture_t p;

	posture_collect(&p);

	if (p.os_build != 0) {
		printf("os         %u.%u.%u\n", p.os_major, p.os_minor, p.os_build);
	} else {
		printf("os         unknown\n");
	}
	printf("firewall   %s\n", state_name(p.firewall));
	printf("antivirus  %s\n", state_name(p.antivirus));

	return 0;
}

/* RFC 4231 의 HMAC-SHA256 테스트 벡터. 서버는 OpenSSL, 여기는 CNG 로 서로 다른
 * 구현을 쓰므로, 양쪽이 같은 표준 벡터를 통과하는지가 "mac 이 서로 맞는다"의 근거다. */
static int selftest(void)
{
	static const struct {
		const char *name;
		uint8_t key[20];
		size_t key_len;
		const char *msg;
		uint8_t want[HMAC_SHA256_LEN];
	} cases[] = {
		{ "rfc4231 case 1",
		  { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b }, 20,
		  "Hi There",
		  { 0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce,
		    0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
		    0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7 } },
		{ "rfc4231 case 2",
		  { 'J', 'e', 'f', 'e' }, 4,
		  "what do ya want for nothing?",
		  { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26,
		    0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
		    0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 } },
	};
	uint8_t got[HMAC_SHA256_LEN];
	int failed = 0;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int ok = hmac_sha256(cases[i].key, cases[i].key_len,
				(const uint8_t *)cases[i].msg, strlen(cases[i].msg), got) == 0 &&
			memcmp(got, cases[i].want, sizeof(got)) == 0;

		printf("hmac-sha256 %-16s %s\n", cases[i].name, ok ? "ok" : "FAIL");
		failed |= !ok;
	}

	return failed;
}

int main(int argc, char *argv[])
{
	tunnel_config_t cfg;
	WSADATA wsa;
	char *end;
	long server_port;
	int ret = 1;

	if (argc == 2 && strcmp(argv[1], "posture") == 0) {
		return print_posture();
	}
	if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
		return selftest();
	}

	if (argc != 5) {
		fprintf(stderr, "usage: %s <adapter-name> <server-ip> <port> <tunnel-ip>\n", argv[0]);
		fprintf(stderr, "       %s posture\n", argv[0]);
		fprintf(stderr, "       %s selftest\n", argv[0]);
		fprintf(stderr, " e.g. %s my_vpn 49.247.139.39 9000 10.0.0.3\n", argv[0]);
		return 1;
	}

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "WSAStartup failed\n");
		return 1;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.prefix_len = TUN_PREFIX_LEN;
	cfg.log_packets = 1;
	cfg.on_log = on_log;

	if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, cfg.adapter_name, TUNNEL_ADAPTER_NAME_LEN) == 0) {
		fprintf(stderr, "invalid adapter name : %s\n", argv[1]);
		goto out;
	}

	if (inet_pton(AF_INET, argv[2], &cfg.server_ip) != 1) {
		fprintf(stderr, "invalid server ip : %s\n", argv[2]);
		goto out;
	}

	server_port = strtol(argv[3], &end, 10);
	if (*end != '\0' || server_port <= 0 || server_port > 65535) {
		fprintf(stderr, "invalid port : %s\n", argv[3]);
		goto out;
	}
	cfg.server_port = (uint16_t)server_port;

	if (inet_pton(AF_INET, argv[4], &cfg.tun_ip) != 1) {
		fprintf(stderr, "invalid tun ip : %s\n", argv[4]);
		goto out;
	}

	/* tunnel_run()이 핸드셰이크 대기와 메인 루프 양쪽에서 보므로 manual-reset */
	stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!stop_event) {
		fprintf(stderr, "CreateEvent failed: %lu\n", GetLastError());
		goto out;
	}
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);

	ret = tunnel_run(&cfg, stop_event);

	CloseHandle(stop_event);

out:
	WSACleanup();
	return ret;
}
