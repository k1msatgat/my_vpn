#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#define htobe64 htonll
#define be64toh ntohll
#else
#include <endian.h>
#include <arpa/inet.h>
#endif

#include "proto.h"

int msg_encode(unsigned char *buf, size_t cap, const struct msg_header *h)
{
	struct msg_header w = *h;

	if (cap < sizeof(w)){
		return -1;
	}

	w.reserved[0] = w.reserved[1] = 0;
	w.session_idx = htonl(h->session_idx);
	memcpy(buf, &w, sizeof(w));

	return (int)sizeof(w);
}

int msg_decode(const unsigned char *buf, size_t len, struct msg_header *h)
{
	if (len < sizeof(*h)){
		return -1;
	}

	memcpy(h, buf, sizeof(*h));
	h->session_idx = ntohl(h->session_idx);

	return (int)sizeof(*h);
}

int data_encode(unsigned char *buf, size_t cap, const struct data_header *d)
{
	uint64_t v = htobe64(d->counter);

	if (cap < sizeof(v)){
		return -1;
	}

	memcpy(buf, &v, sizeof(v));

	return (int)sizeof(v);
}

int data_decode(const unsigned char *buf, size_t len, struct data_header *d)
{
	uint64_t v;

	if (len < sizeof(v)){
		return -1;
	}

	memcpy(&v, buf, sizeof(v));
	d->counter = be64toh(v);

	return (int)sizeof(v);
}
