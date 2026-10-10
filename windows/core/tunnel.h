#ifndef TUNNEL_H
#define TUNNEL_H

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#include "proto.h"

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

typedef enum tunnel_dir {
	TUNNEL_DIR_TX = 0,		/* TUN -> UDP (내가 보냄) */
	TUNNEL_DIR_RX			/* UDP -> TUN (내가 받음) */
} tunnel_dir_t;

/* 패킷을 어떻게 처리했는지. 폐기 이유를 그대로 올려 UI가 집계할 수 있게 한다. */
typedef enum tunnel_verdict {
	TUNNEL_PASS = 0,
	TUNNEL_DROP_NON_IPV4,		/* IPv6 등 */
	TUNNEL_DROP_NON_UNICAST,	/* 멀티캐스트 / 브로드캐스트 — 전달할 피어가 없다 */
	TUNNEL_DROP_TOO_LONG,
	TUNNEL_DROP_NOT_SERVER,		/* 출발지가 서버가 아니다 */
	TUNNEL_DROP_BAD_HEADER,		/* 길이 부족 / 버전 불일치 */
	TUNNEL_DROP_WRONG_SESSION,
	TUNNEL_DROP_UNKNOWN_TYPE,
	TUNNEL_DROP_IO_ERROR		/* 검증은 통과했지만 전달 자체가 실패 */
} tunnel_verdict_t;

/* 상세창 hex 덤프용으로 와이어 앞부분만 떠 둔다 (16B 헤더 + inner 헤더가 보일 정도). */
#define TUNNEL_SNAP_LEN 64

/* 패킷 한 건의 디코드 결과.
 *
 * 포맷된 문자열이 아니라 필드 그대로 올리는 것이 핵심이다. tunnel.c 는 이미
 * 세션 idx, counter, inner src/dst 를 알고 있는데, 이를 문자열로 만들어 넘기면
 * UI 는 그 정보를 되찾을 수 없다. 와이어 포맷을 열로 펼치거나 헤더를 필드별로
 * 분해해 보여주려면 구조체로 올려야 한다. */
typedef struct tunnel_packet {
	uint64_t seq;			/* 1부터 증가 */
	uint64_t t_ms;			/* tunnel_run 시작 기준 경과 시간 */

	uint8_t dir;			/* tunnel_dir_t */
	uint8_t verdict;		/* tunnel_verdict_t */
	uint8_t msg_type;		/* MSG_TYPE_*, 헤더를 못 읽었으면 0 */
	uint8_t has_counter;		/* data_header 가 있었나 */

	uint32_t session_idx;
	uint64_t counter;

	uint32_t wire_len;		/* UDP 페이로드 길이 (우리 헤더 포함) */
	uint32_t inner_len;		/* inner IP 패킷 길이, 없으면 0 */

	struct in_addr inner_src;
	struct in_addr inner_dst;
	uint8_t has_inner;
	uint8_t proto;			/* inner IP protocol */

	uint8_t has_l4;
	uint8_t icmp_type;		/* proto == 1 */
	uint8_t icmp_code;
	uint16_t sport;			/* proto == 6 / 17 */
	uint16_t dport;

	uint8_t snap[TUNNEL_SNAP_LEN];
	uint8_t snap_len;
} tunnel_packet_t;

/* 콜백은 tunnel_run()을 호출한 스레드에서 불린다.
 * GUI에서는 여기서 컨트롤을 직접 건드리지 말고 PostMessage로 넘겨야 한다. */
typedef struct tunnel_config {
	wchar_t adapter_name[TUNNEL_ADAPTER_NAME_LEN];
	struct in_addr server_ip;
	uint16_t server_port;		/* host byte order */
	struct in_addr tun_ip;
	uint8_t prefix_len;		/* 터널 대역 prefix, 예) 24 */
	int log_packets;		/* 0이 아니면 패킷마다 on_log 로 한 줄 */

	void (*on_log)(void *ctx, const char *line);
	void (*on_state)(void *ctx, tunnel_state_t state);

	/* 설정하면 패킷마다 디코드 결과가 올라온다. 이때 log_packets 기반의
	 * 패킷 텍스트 로그는 중복이므로 생략한다. */
	void (*on_packet)(void *ctx, const tunnel_packet_t *pkt);

	void *ctx;
} tunnel_config_t;

/* 터널을 열고 stop_event가 signaled 될 때까지 블로킹으로 돈다.
 * stop_event는 manual-reset 이벤트여야 한다.
 * 정상 종료 0, 초기화/핸드셰이크 실패나 I/O 오류는 1. */
int tunnel_run(const tunnel_config_t *cfg, HANDLE stop_event);

/* 폐기 이유의 짧은 영문 이름.
 * 코어는 표시 문자열을 가지지 않는다 — 여기서 한국어를 돌려주면 char* 가 되어
 * UI 가 ANSI 코드페이지로 변환하면서 깨진다. 화면에 쓸 라벨은 UI 가 enum 을
 * 보고 와이드 문자열로 직접 고른다. 이 함수는 콘솔 로그용이다. */
const char *tunnel_verdict_name(int verdict);

#ifdef __cplusplus
}
#endif

#endif
