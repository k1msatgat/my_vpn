#ifndef PEER_H
#define PEER_H

#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

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
    time_t last_seen;
} peer_t;

void peer_touch(peer_t *peer, const struct sockaddr_in *outer, socklen_t len);
void peer_init(void);
peer_t *peer_register(struct in_addr inner,
                        const struct sockaddr_in *outer, socklen_t len);
peer_t *peer_find_idx(uint32_t idx);
peer_t *peer_lookup(struct in_addr inner);
void peer_dump(void);

#endif
