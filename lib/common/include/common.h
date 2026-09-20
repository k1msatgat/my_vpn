#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>
#define TUN_MTU		1400
#define BUF_SIZE	2048
#define TUN_PROR	9000

#define IP_PROTO_OFF    9
#define IP_SRC_OFF     12
#define IP_DST_OFF     16
#define IP_ADDR_LEN     4
#define IP_MIN_HDR     20

#define IP_VERSION(buf)  ((buf)[0] >> 4)
#define IP_IHL(buf)      (((buf)[0] & 0x0F) * 4)

int tun_alloc(char *dev);
void hex_dump(const unsigned char *buf, int len);
const char *ip_str(struct in_addr a, char *dst);

#endif

