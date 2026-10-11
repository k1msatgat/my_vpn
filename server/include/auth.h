#ifndef AUTH_H
#define AUTH_H

/* 기기 인증에 쓰는 암호 도구. 알고리즘은 직접 구현하지 않고 OpenSSL 에 맡긴다.
 * Windows 클라이언트는 CNG 를 쓰지만 HMAC-SHA256 은 표준이라 같은 입력에 같은
 * 값이 나온다. */

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

/* mac = HMAC-SHA256(key, msg). 성공 0, 실패 -1. */
int auth_hmac(const uint8_t key[AUTH_KEY_LEN], const uint8_t *msg, size_t len,
		uint8_t mac[AUTH_MAC_LEN]);

/* 두 mac 이 같으면 1. 걸리는 시간이 내용에 좌우되지 않는다. */
int auth_mac_equal(const uint8_t a[AUTH_MAC_LEN], const uint8_t b[AUTH_MAC_LEN]);

/* 예측할 수 없는 난수로 채운다. 성공 0, 실패 -1. */
int auth_random(uint8_t *buf, size_t len);

/* RFC 4231 테스트 벡터로 auth_hmac 을 확인한다. 모두 맞으면 0. */
int auth_selftest(void);

#endif
