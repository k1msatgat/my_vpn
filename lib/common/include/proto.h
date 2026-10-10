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

#define PROTOCOL_VERSION 1 //우선 common 에서 서버와 클라이언트 모두 동일 버전사용하도록 설정.

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

typedef enum MSG_TYPE {
	MSG_TYPE_REQ_HANDSHAKE = 1,
	MSG_TYPE_RES_HANDSHAKE,
	MSG_TYPE_DATA,
	MSG_TYPE_KEEPALIVE
} msg_type_t;

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

int msg_encode(unsigned char *buf, size_t cap, const struct msg_header *h);
int msg_decode(const unsigned char *buf, size_t len, struct msg_header *h);

int data_encode(unsigned char *buf, size_t cap, const struct data_header *d);
int data_decode(const unsigned char *buf, size_t len, struct data_header *d);

#ifdef __cplusplus
}
#endif

#endif
