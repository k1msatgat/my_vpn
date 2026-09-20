#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "peer.h"

static struct peer peers[MAX_PEERS];

static const char *ip4(struct in_addr a, char *dst)
{
	return inet_ntop(AF_INET, &a, dst, INET_ADDRSTRLEN);
}

void peer_init(void)
{
	memset(peers, 0, sizeof(peers));
}

struct peer *peer_lookup(struct in_addr inner)
{
	int i;

	for (i = 0; i < MAX_PEERS; i++) {
		if (peers[i].used && peers[i].inner.s_addr == inner.s_addr)
			return &peers[i];
	}
	return NULL;
}

struct peer *peer_learn(struct in_addr inner,
		const struct sockaddr_in *outer, socklen_t len)
{
	char ib[INET_ADDRSTRLEN], ob[INET_ADDRSTRLEN], nb[INET_ADDRSTRLEN];
	struct peer *p;
	int i, victim = 0;

	p = peer_lookup(inner);

	if (p) {
		if (p->outer.sin_addr.s_addr != outer->sin_addr.s_addr ||
				p->outer.sin_port        != outer->sin_port) {
			printf("[peer] rebind %s: %s:%d -> %s:%d\n",
					ip4(inner, ib),
					ip4(p->outer.sin_addr, ob), ntohs(p->outer.sin_port),
					ip4(outer->sin_addr, nb),   ntohs(outer->sin_port));
		}
	} else {
		for (i = 0; i < MAX_PEERS; i++) {
			if (!peers[i].used) {
				victim = i; goto found;
			}
			if (peers[i].last_seen < peers[victim].last_seen) {
				victim = i;
			}
		}

		printf("[peer] table full, evicting %s\n",
				ip4(peers[victim].inner, ob));
found:
		p = &peers[victim];
		p->used  = 1;
		p->inner = inner;
		printf("[peer] learn %s <- %s:%d\n",
				ip4(inner, ib),
				ip4(outer->sin_addr, ob), ntohs(outer->sin_port));
	}

	p->outer     = *outer;
	p->outer_len = len;
	p->last_seen = time(NULL);
	return p;
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

