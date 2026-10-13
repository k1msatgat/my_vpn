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

#define PROTOCOL_VERSION 3 /* 서버와 클라이언트가 같은 값을 쓴다. 2: 기기 인증, 3: 세션 암호화 */

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

/* 기기 인증 + 포스처 보고 + 세션 암호화 (ZTNA).
 *
 * 핸드셰이크는 네 메시지다. 모두 msg_header(8B) 뒤에 아래 페이로드가 붙는다.
 *
 *   C -> S  REQ_HANDSHAKE  tun_ip(4) | device_id(16)
 *   S -> C  CHALLENGE      nonce(16)
 *   C -> S  AUTH           tun_ip(4) | device_id(16) | nonce(16) | pub_c(64) | posture(8) | mac(32)
 *   S -> C  RES_HANDSHAKE  pub_s(64) | tag(16)          헤더의 session_idx 가 세션 번호
 *        또는 DENY          reason(1) | mac(32)          헤더의 session_idx 는 0
 *
 * AUTH 의 mac 은 HMAC-SHA256(기기 키, 헤더부터 mac 앞까지 116B) 이다. 서버가 매번 새로
 * 뽑은 nonce 가 들어가므로 예전 AUTH 를 녹화해 다시 보내도 통과하지 못한다. AUTH 가
 * tun_ip 와 device_id 를 다시 싣는 것은 서버가 출발지 주소로 "진행 중인 핸드셰이크"를
 * 찾지 않아도 되게 하려는 것이다. NAT 뒤에서는 두 메시지 사이에 출발지 포트가 바뀔 수 있다.
 *
 * pub_c / pub_s 는 이번 핸드셰이크에만 쓰고 버리는 ECDH P-256 공개키다. 둘로 합의한
 * 값과 기기 키에서 세션 키를 뽑는다 (secure.h). 기기 키가 들어가므로 그 키를 모르는
 * 쪽은 같은 세션 키에 닿지 못한다 — RES_HANDSHAKE 의 tag 가 맞았다는 것이 곧 "상대가
 * 이 기기의 키를 아는 서버"라는 증명이다. 그래서 클라이언트는 tag 가 맞지 않는 수락을
 * 받지 않는다.
 *
 * DENY 의 mac 은 HMAC-SHA256(기기 키, "my_vpn deny" | reason | AUTH 의 mac) 이다.
 * 서명이 맞는 AUTH 를 받은 뒤의 거절(터널 IP, 포스처)에만 채워지고, 그 전의 거절은
 * 서버가 상대의 키를 확인하지 못한 상태라 0 으로 둔다. 클라이언트는 mac 이 맞는 거절만
 * 서버의 답으로 믿는다.
 *
 * 세션이 열린 뒤의 패킷은 종류와 관계없이 같은 모양이다.
 *
 *   msg_header(8) | counter(8) | 암호문 | tag(16)
 *
 *   DATA       C <-> S   암호문 = inner IPv4 패킷
 *   KEEPALIVE  C  -> S   암호문 = posture(8)       포스처 재보고
 *              S  -> C   암호문 없음
 *   DENY       S  -> C   암호문 = reason(1)        세션 중의 거절. 헤더의 session_idx 는 그 세션
 *
 * AES-256-GCM 이고, 앞의 16B(헤더와 counter)는 암호화하지 않지만 tag 가 함께 덮는다.
 * 방향마다 키가 다르고 counter 가 그대로 nonce 가 된다. */
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

#define ECDH_PUB_LEN	64	/* P-256 공개키 X(32) | Y(32) */
#define AEAD_TAG_LEN	16

#define REQ_HS_PAYLOAD_LEN	(IP_ADDR_LEN + DEVICE_ID_LEN)

/* AUTH 페이로드 안에서의 위치 (헤더 뒤부터). */
#define AUTH_OFF_TUN_IP		0
#define AUTH_OFF_DEVICE_ID	(AUTH_OFF_TUN_IP + IP_ADDR_LEN)
#define AUTH_OFF_NONCE		(AUTH_OFF_DEVICE_ID + DEVICE_ID_LEN)
#define AUTH_OFF_PUB		(AUTH_OFF_NONCE + AUTH_NONCE_LEN)
#define AUTH_OFF_POSTURE	(AUTH_OFF_PUB + ECDH_PUB_LEN)
#define AUTH_OFF_MAC		(AUTH_OFF_POSTURE + sizeof(posture_report_t))
#define AUTH_PAYLOAD_LEN	(AUTH_OFF_MAC + AUTH_MAC_LEN)

/* mac 이 덮는 범위와 패킷 전체. 둘 다 패킷의 첫 바이트(헤더)부터 잰 길이다. */
#define AUTH_SIGNED_LEN		(sizeof(msg_header_t) + AUTH_OFF_MAC)
#define AUTH_PACKET_LEN		(sizeof(msg_header_t) + AUTH_PAYLOAD_LEN)

/* RES_HANDSHAKE: tag 가 덮는 앞부분(헤더 | pub_s)과 패킷의 최소 길이. */
#define RES_HS_HEAD_LEN		(sizeof(msg_header_t) + ECDH_PUB_LEN)
#define RES_HS_MIN_LEN		(RES_HS_HEAD_LEN + AEAD_TAG_LEN)

/* 핸드셰이크 중의 DENY: 헤더 | reason(1) | mac(32) */
#define DENY_HS_LEN		(sizeof(msg_header_t) + 1 + AUTH_MAC_LEN)

/* 세션 패킷에 우리가 덧붙이는 바이트: 헤더(8) + counter(8) + tag(16) */
#define PKT_OVERHEAD		(PKT_HDR_LEN + AEAD_TAG_LEN)

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
