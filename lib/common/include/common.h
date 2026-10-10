#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>
#include <stdint.h>

#include "proto.h"

int tun_alloc(char *dev);
void hex_dump(const unsigned char *buf, int len);
const char *ip_str(struct in_addr a, char *dst);

#endif

