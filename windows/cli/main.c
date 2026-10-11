#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tunnel.h"
#include "posture.h"

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

	if (argc != 5) {
		fprintf(stderr, "usage: %s <adapter-name> <server-ip> <port> <tunnel-ip>\n", argv[0]);
		fprintf(stderr, "       %s posture\n", argv[0]);
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
