#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "acl.h"
#include "common.h"

static struct acl_rule rules[MAX_RULES];
static int             nrules;

void acl_init(void)
{
	memset(rules, 0, sizeof(rules));
	nrules = 0;
}

static int parse_cidr(const char *s, struct in_addr *addr, struct in_addr *mask)
{
	char buf[64];
	char *slash;
	int   bits;

	if (strcmp(s, "any") == 0) {
		addr->s_addr = 0;
		mask->s_addr = 0;
		return 0;
	}

	if (strlen(s) >= sizeof(buf)) {
		return -1;
	}

	strcpy(buf, s);

	slash = strchr(buf, '/');
	if (slash != NULL) {
		*slash = '\0';
		bits = atoi(slash + 1);
		if (bits < 0 || bits > 32) return -1;
	} else {
		bits = 32;
	}

	if (inet_pton(AF_INET, buf, addr) != 1) {
		return -1;
	}

	if (bits == 0) {
		mask->s_addr = 0;
	}
	else {
		mask->s_addr = htonl(0xFFFFFFFFu << (32 - bits));
	}

	addr->s_addr &= mask->s_addr;

	return 0;
}

static int parse_proto(const char *s, unsigned char *proto)
{
	if      (strcmp(s, "any")  == 0) *proto = ACL_ANY_PROTO;
	else if (strcmp(s, "icmp") == 0) *proto = 1;
	else if (strcmp(s, "tcp")  == 0) *proto = 6;
	else if (strcmp(s, "udp")  == 0) *proto = 17;
	else {
		int v = atoi(s);
		if (v < 0 || v > 255) return -1;
		*proto = (unsigned char)v;
	}
	return 0;
}

static int parse_port(const char *s, unsigned short *lo, unsigned short *hi)
{
	char  buf[32];
	char *dash;
	int   a, b;

	if (strcmp(s, "-") == 0 || strcmp(s, "any") == 0) {
		*lo = 0;
		*hi = 65535;
		return 0;
	}

	if (strlen(s) >= sizeof(buf)) return -1;
	strcpy(buf, s);

	dash = strchr(buf, '-');
	if (dash != NULL) {
		*dash = '\0';
		a = atoi(buf);
		b = atoi(dash + 1);
	} else {
		a = b = atoi(buf);
	}

	if (a < 0 || a > 65535 || b < 0 || b > 65535 || a > b) return -1;

	*lo = (unsigned short)a;
	*hi = (unsigned short)b;
	return 0;
}

int acl_load(const char *path)
{
	FILE *fp;
	char  line[256];
	char  s_src[64], s_dst[64], s_proto[16], s_port[32], s_act[16];
	int   lineno = 0;

	fp = fopen(path, "r");
	if (fp == NULL) {
		perror("fopen acl"); return -1;
	}

	acl_init();

	while (fgets(line, sizeof(line), fp) != NULL) {
		struct acl_rule *r;
		char *hash;

		lineno++;

		hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = '\0'; 
		}

		if (sscanf(line, "%63s %63s %15s %31s %15s",
					s_src, s_dst, s_proto, s_port, s_act) != 5) {
			continue;
		}

		if (nrules >= MAX_RULES) {
			fprintf(stderr, "acl: too many rules (max %d)\n", MAX_RULES);
			fclose(fp);
			return -1;
		}

		r = &rules[nrules];

		if (parse_cidr(s_src, &r->src_addr, &r->src_mask) < 0 ||
				parse_cidr(s_dst, &r->dst_addr, &r->dst_mask) < 0 ||
				parse_proto(s_proto, &r->proto) < 0 ||
				parse_port(s_port, &r->dport_lo, &r->dport_hi) < 0) {
			fprintf(stderr, "acl: parse error at line %d\n", lineno);
			fclose(fp);
			return -1;
		}

		if      (strcmp(s_act, "allow") == 0) {
			r->action = ACL_ALLOW;
		}
		else if (strcmp(s_act, "deny")  == 0) {
			r->action = ACL_DENY;
		}
		else {
			fprintf(stderr, "acl: unknown action '%s' at line %d\n", s_act, lineno);
			fclose(fp);
			return -1;
		}

		nrules++;
	}

	fclose(fp);
	return nrules;
}

int acl_check(const unsigned char *pkt, int len)
{
	struct in_addr src, dst;
	unsigned char  proto;
	unsigned short dport = 0;
	int            ihl, i;

	if (len < IP_MIN_HDR) {
		return ACL_DENY; 
	}

	ihl = IP_IHL(pkt);
	if (ihl < IP_MIN_HDR || ihl > len) {
		return ACL_DENY;
	}

	memcpy(&src, pkt + IP_SRC_OFF, IP_ADDR_LEN);
	memcpy(&dst, pkt + IP_DST_OFF, IP_ADDR_LEN);
	proto = pkt[IP_PROTO_OFF];

	if (proto == IPPROTO_TCP || proto == IPPROTO_UDP) {
		if (len >= ihl + L4_DPORT_OFF + L4_PORT_LEN) {
			memcpy(&dport, pkt + ihl + L4_DPORT_OFF, L4_PORT_LEN);
		}
		else {
			return ACL_DENY;
		}
		dport = ntohs(dport);
	}

	for (i = 0; i < nrules; i++) {
		struct acl_rule *r = &rules[i];

		if ((src.s_addr & r->src_mask.s_addr) != r->src_addr.s_addr) {
			continue;
		}
		if ((dst.s_addr & r->dst_mask.s_addr) != r->dst_addr.s_addr) {
			continue;
		}

		if (r->proto != ACL_ANY_PROTO && r->proto != proto) {
			continue;
		}

		if (proto == IPPROTO_TCP || proto == IPPROTO_UDP) {
			if (dport < r->dport_lo || dport > r->dport_hi) continue;
		}

		return r->action;
	}

	return ACL_DENY;
}

static int mask_bits(struct in_addr mask)
{
	unsigned int m = ntohl(mask.s_addr);
	int n = 0;

	while (m & 0x80000000u) {
		n++; m <<= 1;
	}
	return n;
}

void acl_dump(void)
{
	char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
	int  i;

	printf("--- acl (%d rules) ---\n", nrules);
	for (i = 0; i < nrules; i++) {
		struct acl_rule *r = &rules[i];

		inet_ntop(AF_INET, &r->src_addr, a, sizeof(a));
		inet_ntop(AF_INET, &r->dst_addr, b, sizeof(b));

		printf("[%d] %s/%d -> %s/%d proto=%d dport=%u-%u %s\n", i,
				a, mask_bits(r->src_mask),
				b, mask_bits(r->dst_mask),
				r->proto, r->dport_lo, r->dport_hi,
				r->action == ACL_ALLOW ? "allow" : "deny");
	}
}

