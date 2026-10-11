#ifndef HMAC_H
#define HMAC_H

/* HMAC-SHA256.
 *
 * 기기 인증의 mac 을 만드는 데 쓴다. "키를 아는 쪽만 이 메시지를 만들 수 있다"를
 * 증명하는 값이고, 키 자체는 와이어에 나가지 않는다. 그냥 SHA256(key | msg) 로
 * 만들면 길이 확장 공격으로 msg 뒤에 내용을 덧붙인 유효한 값을 키 없이 만들 수
 * 있어서 HMAC 구조를 쓴다. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HMAC_SHA256_LEN 32

/* 성공 0, 실패 -1. */
int hmac_sha256(const uint8_t *key, size_t key_len,
		const uint8_t *msg, size_t msg_len,
		uint8_t out[HMAC_SHA256_LEN]);

#ifdef __cplusplus
}
#endif

#endif
