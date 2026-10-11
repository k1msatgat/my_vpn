#ifndef DEVICE_H
#define DEVICE_H

/* 등록된 기기 목록.
 *
 * 지금까지 서버는 터널 IP 를 자칭한 누구에게나 세션을 줬다. 이제 관리자가 미리
 * 등록해 둔 기기만 받는다. 기기마다 ID(이름표), 키(그 기기와 서버만 아는 비밀),
 * 배정된 터널 IP 를 가진다. 값은 기기에서 `vpn_client enroll` (또는 GUI 의
 * [기기 등록...])로 만들고 관리자가 이 파일에 옮겨 적는다.
 *
 *   # id(32 hex)                      key(64 hex)   tunnel-ip
 *   2bd5d6e0610d2b7c07f884d0c5afb043  a24d4c3d...   10.0.0.3
 */

#include <stdint.h>
#include <netinet/in.h>

#include "proto.h"

#define MAX_DEVICES 16

typedef struct device {
	uint8_t id[DEVICE_ID_LEN];
	uint8_t key[AUTH_KEY_LEN];
	struct in_addr tun_ip;
} device_t;

void device_init(void);

/* 읽은 기기 수를 돌려준다. 파일을 못 읽거나 형식이 틀리면 -1. */
int device_load(const char *path);

device_t *device_find(const uint8_t id[DEVICE_ID_LEN]);

/* 로그에 쓸 ID 문자열. dst 는 DEVICE_ID_STRLEN 이상이어야 한다. */
#define DEVICE_ID_STRLEN (DEVICE_ID_LEN * 2 + 1)
const char *device_id_str(const uint8_t id[DEVICE_ID_LEN], char *dst);

void device_dump(void);

#endif
