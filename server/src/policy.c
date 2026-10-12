#include <stdlib.h>
#include <string.h>

#include "policy.h"

/* 모르는 단어는 무시하지 않고 실패시킨다. "firewal=on" 같은 오타가 조용히 지나가면
 * 관리자는 걸었다고 믿는 정책이 실제로는 걸리지 않는다. */
int posture_req_parse(const char *tok, posture_req_t *req)
{
	char *end;
	unsigned long v;

	if (strcmp(tok, "firewall=on") == 0) {
		req->need_firewall = 1;
		return 0;
	}
	if (strcmp(tok, "antivirus=on") == 0) {
		req->need_antivirus = 1;
		return 0;
	}
	if (strncmp(tok, "min-build=", 10) == 0 && tok[10] != '\0') {
		v = strtoul(tok + 10, &end, 10);
		if (*end != '\0' || v > 0xFFFFFFFFul) {
			return -1;
		}
		req->min_build = (uint32_t)v;
		return 0;
	}

	return -1;
}

/* "모름"은 통과가 아니다. 클라이언트가 값을 읽지 못해 POSTURE_STATE_UNKNOWN 을
 * 보냈을 때 통과시키면, 조회를 실패하게 만드는 것만으로 정책을 피할 수 있다.
 * os_build 도 읽지 못하면 0 이라 min-build 조건에 저절로 걸린다. */
int posture_req_ok(const posture_req_t *req, const posture_report_t *posture)
{
	if (req->need_firewall && posture->firewall != POSTURE_STATE_ON) {
		return 0;
	}
	if (req->need_antivirus && posture->antivirus != POSTURE_STATE_ON) {
		return 0;
	}
	if (posture->os_build < req->min_build) {
		return 0;
	}

	return 1;
}

int posture_req_empty(const posture_req_t *req)
{
	return !req->need_firewall && !req->need_antivirus && req->min_build == 0;
}
