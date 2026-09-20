#ifndef ACL_H
#define ACL_H

#include <netinet/in.h>

#define MAX_RULES     32
#define ACL_ANY_PROTO 0

enum acl_action {
    ACL_DENY  = 0,
    ACL_ALLOW = 1
};

struct acl_rule {
    struct in_addr src_addr;
    struct in_addr src_mask;
    struct in_addr dst_addr;
    struct in_addr dst_mask;
    unsigned char  proto;  
    unsigned short dport_lo;
    unsigned short dport_hi;
    int            action;
};

void acl_init(void);
int  acl_load(const char *path);
int  acl_check(const unsigned char *pkt, int len);
void acl_dump(void);

#endif /* ACL_H */
