#ifndef PROTO_H
#define PROTO_H

/* 와이어 프로토콜 정의. Linux(client/server)와 Windows 클라이언트가 함께 쓰므로
 * OS 전용 헤더를 포함하지 않는다. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define PROTO_STATIC_ASSERT static_assert
#else
#define PROTO_STATIC_ASSERT _Static_assert
#endif

#define PROTOCOL_VERSION 2 //우선 common 에서 서버와 클라이언트 모두 동일 버전사용하도록 설정. 2: 기기 인증 핸드셰이크

#define TUN_MTU		1400
#define BUF_SIZE	2048
#define TUN_PROR	9000

#define IP_PROTO_OFF    9
#define IP_SRC_OFF     12
#define IP_DST_OFF     16
#define IP_ADDR_LEN     4
#define IP_MIN_HDR     20

#define L4_SPORT_OFF   0
#define L4_DPORT_OFF   2
#define L4_PORT_LEN    2

#define IP_VERSION(buf)  ((buf)[0] >> 4)
#define IP_IHL(buf)      (((buf)[0] & 0x0F) * 4)

#define PKT_HDR_LEN (sizeof(msg_header_t) + sizeof(data_header_t))

#define KEEPALIVE_INTERVAL_SEC 10
#define KEEPALIVE_INTERVAL_MSEC (KEEPALIVE_INTERVAL_SEC * 1000)
#define SESSION_TIMEOUT_SEC 60
#define SERVER_TIMEOUT_SEC 30

/* 기기 인증 + 포스처 보고 (ZTNA).
 *
 * 핸드셰이크는 네 메시지다. 모두 msg_header(8B) 뒤에 아래 페이로드가 붙는다.
 *
 *   C -> S  REQ_HANDSHAKE  tun_ip(4) | device_id(16)
 *   S -> C  CHALLENGE      nonce(16)
 *   C -> S  AUTH           tun_ip(4) | device_id(16) | nonce(16) | posture(8) | mac(32)
 *   S -> C  RES_HANDSHAKE  (페이로드 없음, 헤더의 session_idx 가 답)
 *        또는 DENY          reason(1)
 *
 * AUTH 의 mac 은 HMAC-SHA256(기기 키, mac 앞까지의 AUTH 페이로드 44B) 이다.
 * 서버가 매번 새로 뽑은 nonce 가 들어가므로 예전 AUTH 를 녹화해 다시 보내도
 * 통과하지 못한다. AUTH 가 tun_ip 와 device_id 를 다시 싣는 것은 서버가 출발지
 * 주소로 "진행 중인 핸드셰이크"를 찾지 않아도 되게 하려는 것이다. NAT 뒤에서는
 * 두 메시지 사이에 출발지 포트가 바뀔 수 있다.
 *
 * 세션이 열린 뒤 클라이언트의 KEEPALIVE 는 포스처를 다시 싣는다.
 *
 *   C -> S  KEEPALIVE      counter(8) | posture(8) | mac(32)
 *   S -> C  KEEPALIVE      (페이로드 없음)
 *
 * 이때 mac 은 HMAC-SHA256(기기 키, msg_header(8) | counter(8) | posture(8)) 이다.
 * 헤더를 넣어 session_idx 에 묶는다. */
typedef enum MSG_TYPE {
	MSG_TYPE_REQ_HANDSHAKE = 1,
	MSG_TYPE_RES_HANDSHAKE,
	MSG_TYPE_DATA,
	MSG_TYPE_KEEPALIVE,
	MSG_TYPE_CHALLENGE,
	MSG_TYPE_AUTH,
	MSG_TYPE_DENY
} msg_type_t;

#define DEVICE_ID_LEN	16
#define AUTH_KEY_LEN	32
#define AUTH_NONCE_LEN	16
#define AUTH_MAC_LEN	32	/* HMAC-SHA256 출력 전체 */

typedef enum DENY_REASON {
	DENY_UNKNOWN_DEVICE = 1,	/* 등록되지 않은 device_id */
	DENY_BAD_AUTH,			/* nonce 불일치 / 만료, mac 불일치 */
	DENY_TUN_IP,			/* 그 기기에 배정된 터널 IP 가 아니다 */
	DENY_POSTURE			/* 인증은 됐지만 포스처가 정책에 못 미친다 */
} deny_reason_t;

/* 포스처 항목의 와이어 값. 0 이 "모름"이라 비워 둔 필드가 통과로 읽히지 않는다. */
typedef enum POSTURE_STATE {
	POSTURE_STATE_UNKNOWN = 0,
	POSTURE_STATE_OFF,
	POSTURE_STATE_ON
} posture_state_wire_t;

typedef struct posture_report {
	uint32_t os_build;	/* 못 읽었으면 0 */
	uint8_t firewall;	/* POSTURE_STATE_* */
	uint8_t antivirus;	/* POSTURE_STATE_* */
	uint8_t reserved[2];
} posture_report_t;

#define REQ_HS_PAYLOAD_LEN	(IP_ADDR_LEN + DEVICE_ID_LEN)
#define AUTH_SIGNED_LEN		(IP_ADDR_LEN + DEVICE_ID_LEN + AUTH_NONCE_LEN + sizeof(posture_report_t))
#define AUTH_PAYLOAD_LEN	(AUTH_SIGNED_LEN + AUTH_MAC_LEN)
#define KA_SIGNED_LEN		(sizeof(msg_header_t) + sizeof(data_header_t) + sizeof(posture_report_t))

typedef struct msg_header {
	uint8_t version;
	uint8_t type;
	uint8_t reserved[2];
	uint32_t session_idx;
} msg_header_t;

typedef struct data_header {
	uint64_t counter;
} data_header_t;

PROTO_STATIC_ASSERT(sizeof(msg_header_t) == 8, "msg_header wire size");
PROTO_STATIC_ASSERT(sizeof(data_header_t) == 8, "data_header wire size");
PROTO_STATIC_ASSERT(sizeof(posture_report_t) == 8, "posture_report wire size");

int msg_encode(unsigned char *buf, size_t cap, const struct msg_header *h);
int msg_decode(const unsigned char *buf, size_t len, struct msg_header *h);

int data_encode(unsigned char *buf, size_t cap, const struct data_header *d);
int data_decode(const unsigned char *buf, size_t len, struct data_header *d);

int posture_encode(unsigned char *buf, size_t cap, const struct posture_report *p);
int posture_decode(const unsigned char *buf, size_t len, struct posture_report *p);

#ifdef __cplusplus
}
#endif

#endif
