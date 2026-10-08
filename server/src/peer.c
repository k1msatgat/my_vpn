#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

#include "peer.h"

static peer_t peers[MAX_PEERS];

static const char *ip4(struct in_addr a, char *dst)
{
	return inet_ntop(AF_INET, &a, dst, INET_ADDRSTRLEN);
}

static uint32_t make_idx(int32_t slot)
{
	uint32_t random;
	uint32_t idx;

	do {
		if (getrandom(&random, sizeof(random), 0) != sizeof(random)) {
			return 0;
		}

		idx = (random & ~IDX_SLOT_MASK) | (uint32_t)slot;

	} while(idx == 0);
	return idx;
}

peer_t *peer_find_idx(int32_t idx)
{
	uint32_t slot = idx & IDX_SLOT_MASK;

	if (slot >= MAX_PEERS){
		return NULL;
	}

	if (!peers[slot].used || peers[slot].session_idx != idx){
		return NULL;
	}

	return &peers[slot];
}

peer_t *peer_register(struct in_addr inner, const struct sockaddr_in *outer, socklen_t len)
{
	char ib[INET_ADDRSTRLEN];
	char ob[INET_ADDRSTRLEN];
	peer_t *peer;
	uint32_t idx;
	int32_t i;
	int32_t slot = -1;
	int32_t victim = 0;

	peer = peer_lookup(inner);
	if (peer) {
		if(peer->outer.sin_addr.s_addr == outer->sin_addr.s_addr && peer->outer.sin_port == outer->sin_port ){
			peer->last_seen = time(NULL);
			return peer;
		}
		slot = (int32_t)(peer - peers);
	} else {
		for (i = 0; i < MAX_PEERS; i++){
			if (!peers[i].used) {
				slot = i;
				break;
			}
			if (peers[i].last_seen < peers[victim].last_seen) {
				victim = i;
			}
		}

		if (slot < 0) {
			printf("[peer] table full evicting %s\n", ip4(peers[victim].inner, ib));
			slot = victim;
		}
	}

	idx = make_idx(slot);
	if (idx == 0) {
		return NULL;
	}

	peer = &peers[slot];
	memset(peer, 0, sizeof(*peer));
	peer->used = 1;
	peer->session_idx = idx;
	peer->inner = inner;
	peer->outer = *outer;
	peer->outer_len = len;
	peer->last_seen = time(NULL);

	printf("[peer] register %s <- %s:%d idx=%08x\n",
			ip4(inner, ib), ip4(outer->sin_addr, ob),
			ntohs(outer->sin_port), idx);

	return peer;
}

void peer_init(void)
{
	memset(peers, 0, sizeof(peers));
}

peer_t *peer_lookup(struct in_addr inner)
{
	int i;

	for (i = 0; i < MAX_PEERS; i++) {
		if (peers[i].used && peers[i].inner.s_addr == inner.s_addr)
			return &peers[i];
	}
	return NULL;
}

void peer_dump(void)
{
	char ib[INET_ADDRSTRLEN], ob[INET_ADDRSTRLEN];
	int i;

	printf("--- peer table ---\n");
	for (i = 0; i < MAX_PEERS; i++) {
		if (!peers[i].used) continue;
		printf("  [%d] %-12s <- %s:%d  (age %lds)\n", i,
				ip4(peers[i].inner, ib),
				ip4(peers[i].outer.sin_addr, ob),
				ntohs(peers[i].outer.sin_port),
				(long)(time(NULL) - peers[i].last_seen));
	}
	printf("------------------\n");
}

