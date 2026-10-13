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

int posture_encode(unsigned char *buf, size_t cap, const struct posture_report *p)
{
	struct posture_report w = *p;

	if (cap < sizeof(w)){
		return -1;
	}

	w.reserved[0] = w.reserved[1] = 0;
	w.os_build = htonl(p->os_build);
	memcpy(buf, &w, sizeof(w));

	return (int)sizeof(w);
}

int posture_decode(const unsigned char *buf, size_t len, struct posture_report *p)
{
	if (len < sizeof(*p)){
		return -1;
	}

	memcpy(p, buf, sizeof(*p));
	p->os_build = ntohl(p->os_build);

	return (int)sizeof(*p);
}

static int prefix_ok(uint32_t addr_be, uint8_t prefix_len)
{
	uint32_t host_bits;

	if (prefix_len > 32) {
		return 0;
	}

	/* 대역의 주소에는 prefix 밖의 비트가 없어야 한다. 10.1.2.3/16 같은 값을 그대로
	 * 받으면 같은 대역이 다른 값으로 두 번 적힐 수 있다. */
	host_bits = prefix_len == 0 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu << (32 - prefix_len));
	if (prefix_len == 32) {
		host_bits = 0;
	}

	return (ntohl(addr_be) & host_bits) == 0;
}

int net_config_encode(unsigned char *buf, size_t cap, const net_config_t *c)
{
	uint16_t mtu = htons(c->mtu);
	size_t len = NET_CONFIG_HEAD_LEN + (size_t)c->n_routes * NET_ROUTE_LEN;
	unsigned char *p = buf;
	int i;

	if (c->n_routes > NET_MAX_ROUTES || c->prefix_len > 32 || cap < len) {
		return -1;
	}

	memcpy(p, &c->tun_ip, 4);
	p[4] = c->prefix_len;
	p[5] = c->n_routes;
	memcpy(p + 6, &mtu, 2);
	p += NET_CONFIG_HEAD_LEN;

	for (i = 0; i < c->n_routes; i++) {
		if (!prefix_ok(c->routes[i].addr, c->routes[i].prefix_len)) {
			return -1;
		}
		memcpy(p, &c->routes[i].addr, 4);
		p[4] = c->routes[i].prefix_len;
		p += NET_ROUTE_LEN;
	}

	return (int)len;
}

int net_config_decode(const unsigned char *buf, size_t len, net_config_t *c)
{
	const unsigned char *p = buf;
	uint16_t mtu;
	int i;

	memset(c, 0, sizeof(*c));

	if (len < NET_CONFIG_HEAD_LEN) {
		return -1;
	}

	memcpy(&c->tun_ip, p, 4);
	c->prefix_len = p[4];
	c->n_routes = p[5];
	memcpy(&mtu, p + 6, 2);
	c->mtu = ntohs(mtu);
	p += NET_CONFIG_HEAD_LEN;

	/* 길이는 개수와 정확히 맞아야 한다. 남는 바이트를 못 본 척하면 다음 버전이 뒤에
	 * 덧붙인 것을 이 버전이 조용히 무시하게 된다. */
	if (c->prefix_len > 32 || c->n_routes > NET_MAX_ROUTES ||
			len != NET_CONFIG_HEAD_LEN + (size_t)c->n_routes * NET_ROUTE_LEN) {
		return -1;
	}

	for (i = 0; i < c->n_routes; i++) {
		memcpy(&c->routes[i].addr, p, 4);
		c->routes[i].prefix_len = p[4];
		if (!prefix_ok(c->routes[i].addr, c->routes[i].prefix_len)) {
			return -1;
		}
		p += NET_ROUTE_LEN;
	}

	return 0;
}

int net_config_equal(const net_config_t *a, const net_config_t *b)
{
	int i;

	if (a->tun_ip != b->tun_ip || a->prefix_len != b->prefix_len ||
			a->mtu != b->mtu || a->n_routes != b->n_routes) {
		return 0;
	}

	for (i = 0; i < a->n_routes; i++) {
		if (a->routes[i].addr != b->routes[i].addr ||
				a->routes[i].prefix_len != b->routes[i].prefix_len) {
			return 0;
		}
	}

	return 1;
}
