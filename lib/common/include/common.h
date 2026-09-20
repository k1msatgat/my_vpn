#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>
#define TUN_MTU		1400
#define BUF_SIZE	2048
#define TUN_PROR	9000

int tun_alloc(char *dev);
void hex_dump(const unsigned char *buf, int len);
const char *ip_str(struct in_addr a, char *dst);

#endif

