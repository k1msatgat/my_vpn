#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "acl.h"
#include "common.h"

static struct acl_rule rules[MAX_RULES];
static int             nrules;

/* 파일은 여기에 먼저 읽고, 끝까지 문제없을 때만 위의 표로 옮긴다 (device.c 와 같다).
 * 서버가 도는 중에 다시 읽다가 중간 줄에서 실패하면, 앞의 몇 줄만 들어간 표가 남는다.
 * 기본이 거부라 대개는 닫히는 쪽이지만, 뒤에 있던 deny 가 빠지고 넓은 allow 만 남는
 * 경우에는 열린다. */
static struct acl_rule staged[MAX_RULES];
static int             nstaged;

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
	char *tok;
	int   lineno = 0;
	int   nf, used;

	fp = fopen(path, "r");
	if (fp == NULL) {
		perror("fopen acl"); return -1;
	}

	nstaged = 0;

	while (fgets(line, sizeof(line), fp) != NULL) {
		struct acl_rule *r;
		char *hash;

		lineno++;

		hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = '\0'; 
		}

		/* 빈 줄과 주석만 있는 줄은 넘어간다. 그 밖에 필드가 모자란 줄은 실패다.
		 * 조용히 건너뛰면 deny 한 줄이 빠진 채로 뒤의 넓은 allow 가 적용된다. */
		nf = sscanf(line, "%63s %63s %15s %31s %15s%n",
				s_src, s_dst, s_proto, s_port, s_act, &used);
		if (nf <= 0) {
			continue;
		}
		if (nf != 5) {
			fprintf(stderr, "acl: line %d: expected <src> <dst> <proto> <dport> <action> [requirements]\n", lineno);
			fclose(fp);
			return -1;
		}

		if (nstaged >= MAX_RULES) {
			fprintf(stderr, "acl: too many rules (max %d)\n", MAX_RULES);
			fclose(fp);
			return -1;
		}

		r = &staged[nstaged];
		memset(r, 0, sizeof(*r));

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

		/* action 뒤에 남은 단어들이 포스처 조건이다. 기기 목록과 같은 문법이다. */
		for (tok = strtok(line + used, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n")) {
			if (posture_req_parse(tok, &r->require) < 0) {
				fprintf(stderr, "acl: line %d: unknown requirement '%s'\n", lineno, tok);
				fclose(fp);
				return -1;
			}
		}

		/* 조건은 "허용받으려면 이래야 한다"는 뜻이다. deny 에 붙으면 "방화벽이 켜져
		 * 있을 때만 막는다"가 되는데, 그렇게 읽히길 바라고 쓴 사람은 없을 것이다. */
		if (r->action == ACL_DENY && !posture_req_empty(&r->require)) {
			fprintf(stderr, "acl: line %d: requirements are only for allow rules\n", lineno);
			fclose(fp);
			return -1;
		}

		nstaged++;
	}

	fclose(fp);

	memcpy(rules, staged, sizeof(rules));
	nrules = nstaged;

	return nrules;
}

int acl_check(const unsigned char *pkt, int len, const posture_report_t *posture)
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

		/* 주소와 포트로 규칙이 정해지고, 조건은 그 규칙 안에서 본다. 조건에 못 미쳤다고
		 * 다음 규칙으로 넘어가지 않는다 — 넘어가면 아래쪽의 넓은 allow 가 받아 줘서,
		 * "이 포트는 방화벽이 켜진 기기만"이라고 쓴 줄이 아무것도 막지 못한다. */
		if (r->action == ACL_ALLOW && !posture_req_ok(&r->require, posture)) {
			return ACL_DENY_POSTURE;
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

int acl_count(void)
{
	return nrules;
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

		printf("[%d] %s/%d -> %s/%d proto=%d dport=%u-%u %s", i,
				a, mask_bits(r->src_mask),
				b, mask_bits(r->dst_mask),
				r->proto, r->dport_lo, r->dport_hi,
				r->action == ACL_ALLOW ? "allow" : "deny");
		if (!posture_req_empty(&r->require)) {
			printf("  require: firewall=%s antivirus=%s min-build=%u",
					r->require.need_firewall ? "on" : "-",
					r->require.need_antivirus ? "on" : "-",
					r->require.min_build);
		}
		printf("\n");
	}
}

