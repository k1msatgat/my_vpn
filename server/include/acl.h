#ifndef ACL_H
#define ACL_H

#include <netinet/in.h>

#include "proto.h"
#include "policy.h"

#define MAX_RULES     32
#define ACL_ANY_PROTO 0

/* 규칙의 action 은 ACL_DENY / ACL_ALLOW 둘뿐이다. ACL_DENY_POSTURE 는 acl_check 의
 * 판정으로만 나온다 — 허용 규칙에 걸렸지만 그 규칙이 요구하는 포스처에 못 미쳤다. */
enum acl_action {
    ACL_DENY  = 0,
    ACL_ALLOW = 1,
    ACL_DENY_POSTURE = 2
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

    /* 이 규칙으로 허용받으려면 채워야 하는 포스처 (allow 규칙에만 붙는다). */
    posture_req_t  require;
};

void acl_init(void);
/* 읽은 규칙 수를 돌려준다. 파일을 못 읽거나 형식이 틀리면 -1 이고, 그때는 이전에
 * 읽어 둔 규칙이 그대로 남는다. */
int  acl_load(const char *path);
/* posture 는 이 패킷을 보낸 세션이 마지막으로 보고한 값이다. */
int  acl_check(const unsigned char *pkt, int len, const posture_report_t *posture);
void acl_dump(void);
int  acl_count(void);

#endif /* ACL_H */
