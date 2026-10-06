#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>
#include <stdint.h>
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

typedef enum MSG_TYPE {
	MSG_REQ_HANDSHAKE = 1,
	MSG_RES_HANDSHAKE,
	MSG_DATA,
	MSG_KEEPALIVE
} msg_type_t;

struct msg_header {
	uint8_t version;
	uint8_t type;
	uint8_t reserved[2];
	uint32_t session_idx;
};

struct data_header {
	uint64_t counter;
};


int tun_alloc(char *dev);
void hex_dump(const unsigned char *buf, int len);
const char *ip_str(struct in_addr a, char *dst);

int msg_encode(unsigned char *buf, size_t cap, const struct msg_header *h);
int msg_decode(const unsigned char *buf, size_t len, struct msg_header *h);

int data_encode(unsigned char *buf, size_t cap, const struct data_header *d);
int data_decode(const unsigned char *buf, size_t len, struct data_header *d);

#endif

