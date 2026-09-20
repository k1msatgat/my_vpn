#ifndef PEER_H
#define PEER_H

#include <time.h>
#include <netinet/in.h>

#define MAX_PEERS 8

struct peer {
    int                used;       
    struct in_addr     inner;      
    struct sockaddr_in outer;      
    socklen_t          outer_len;
    time_t             last_seen;  
};

void         peer_init(void);
struct peer *peer_learn(struct in_addr inner,
                        const struct sockaddr_in *outer, socklen_t len);
struct peer *peer_lookup(struct in_addr inner);
void         peer_dump(void);

#endif 
