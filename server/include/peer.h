#ifndef PEER_H
#define PEER_H

#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#include "proto.h"

#define MAX_PEERS 8
#define IDX_SLOT_BIT 8
#define IDX_SLOT_MASK ((1u << IDX_SLOT_BIT) -1)

_Static_assert(MAX_PEERS <= (1 << IDX_SLOT_BIT), "MAX_PEERS exceeds slot bits");

typedef struct peer {
    int32_t used;
    uint32_t session_idx;
    uint64_t tx_counter;
    struct in_addr inner;
    struct sockaddr_in outer;
    socklen_t outer_len;
    time_t last_seen;	/* CLOCK_MONOTONIC 초 */

    /* 이 세션을 연 기기. 포인터가 아니라 ID 로 들고 있다가 쓸 때마다 목록에서 찾는다.
     * 목록에서 지워진 기기는 다음 keepalive 에서 찾지 못해 세션이 끊긴다. */
    uint8_t device_id[DEVICE_ID_LEN];

    /* 마지막으로 받아들인 keepalive 의 counter. 예전 포스처 보고의 재전송을 걸러낸다. */
    uint64_t ka_counter;
    int32_t ka_seen;
} peer_t;

void peer_check_expired(void);
void peer_touch(peer_t *peer, const struct sockaddr_in *outer, socklen_t len);
void peer_init(void);
peer_t *peer_register(struct in_addr inner,
                        const struct sockaddr_in *outer, socklen_t len);
peer_t *peer_find_idx(uint32_t idx);
void peer_drop(peer_t *peer);
peer_t *peer_lookup(struct in_addr inner);
void peer_dump(void);

#endif
