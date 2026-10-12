#ifndef POLICY_H
#define POLICY_H

/* 포스처 조건. "이 상태여야 한다"를 적는 한 가지 문법을 두 곳에서 쓴다.
 *
 *   기기 목록   그 기기를 터널에 들일 것인가        (device.c)
 *   ACL 규칙    들어온 뒤 그 목적지에 가도 되는가   (alc.c)
 *
 *   firewall=on   antivirus=on   min-build=N
 *
 * 적은 것만 검사한다. 아무것도 적지 않은 조건은 누구나 채운다.
 */

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

typedef struct posture_req {
	uint8_t need_firewall;
	uint8_t need_antivirus;
	uint32_t min_build;
} posture_req_t;

/* "firewall=on" 같은 단어 하나를 req 에 더한다. 모르는 단어면 -1. */
int posture_req_parse(const char *tok, posture_req_t *req);

/* 보고된 포스처가 조건을 채우면 1. */
int posture_req_ok(const posture_req_t *req, const posture_report_t *posture);

/* 조건이 하나도 없으면 1. */
int posture_req_empty(const posture_req_t *req);

#endif
