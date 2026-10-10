#ifndef TUNNEL_H
#define TUNNEL_H

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TUNNEL_ADAPTER_NAME_LEN 64

typedef enum tunnel_state {
	TUNNEL_STATE_STOPPED = 0,
	TUNNEL_STATE_CONNECTING,	/* 어댑터 생성 ~ 첫 핸드셰이크 */
	TUNNEL_STATE_CONNECTED,
	TUNNEL_STATE_RECONNECTING	/* 서버 무응답으로 재핸드셰이크 중 */
} tunnel_state_t;

/* 콜백은 tunnel_run()을 호출한 스레드에서 불린다.
 * GUI에서는 여기서 컨트롤을 직접 건드리지 말고 PostMessage로 넘겨야 한다. */
typedef struct tunnel_config {
	wchar_t adapter_name[TUNNEL_ADAPTER_NAME_LEN];
	struct in_addr server_ip;
	uint16_t server_port;		/* host byte order */
	struct in_addr tun_ip;
	uint8_t prefix_len;		/* 터널 대역 prefix, 예) 24 */
	int log_packets;		/* 0이 아니면 패킷마다 로그 */

	void (*on_log)(void *ctx, const char *line);
	void (*on_state)(void *ctx, tunnel_state_t state);
	void *ctx;
} tunnel_config_t;

/* 터널을 열고 stop_event가 signaled 될 때까지 블로킹으로 돈다.
 * stop_event는 manual-reset 이벤트여야 한다.
 * 정상 종료 0, 초기화/핸드셰이크 실패나 I/O 오류는 1. */
int tunnel_run(const tunnel_config_t *cfg, HANDLE stop_event);

#ifdef __cplusplus
}
#endif

#endif
