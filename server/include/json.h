#ifndef JSON_H
#define JSON_H

/* JWT 와 JWKS 를 읽는 데 필요한 만큼의 JSON.
 *
 * 문자열을 한 번 훑어 토큰 배열을 만들고, 값은 필요할 때 원문에서 꺼낸다. 트리를 만들어
 * 메모리를 따로 잡지 않는다 — 부르는 쪽이 토큰 배열의 크기를 정하므로 바깥에서 온 입력이
 * 아무리 커도 쓰는 메모리는 그 안이다.
 *
 * 토큰 i 의 하위 토큰은 바로 뒤(i + 1)부터 시작하고, next 는 그 값 전체를 건너뛴 다음
 * 토큰의 번호다. 객체의 하위 토큰은 키(문자열)와 값이 번갈아 온다.
 *
 * 문법은 엄격하게 본다. 받아 준 JSON 을 다른 구현(IdP, 다른 검증기)이 다르게 읽으면 같은
 * 토큰이 곳에 따라 다른 뜻이 된다. 그래서 뒤에 남는 글자, 끝의 쉼표, 따옴표 없는 키,
 * 제어 문자가 그대로 든 문자열은 실패시키고, 한 객체 안의 같은 키도 실패시킨다 (둘 중
 * 어느 것을 쓰는지가 구현마다 다르다). */

#include <stddef.h>
#include <stdint.h>

typedef enum json_type {
	JSON_OBJECT = 1,
	JSON_ARRAY,
	JSON_STRING,	/* start / end 는 따옴표 안쪽 */
	JSON_PRIMITIVE	/* 숫자, true, false, null */
} json_type_t;

typedef struct json_tok {
	uint8_t type;
	int start;
	int end;
	int size;	/* 객체: 키-값 쌍의 수, 배열: 원소 수 */
	int next;
} json_tok_t;

/* 토큰 수를 돌려준다. 문법이 틀렸거나, 토큰이 max 를 넘거나, 너무 깊으면 -1. */
int json_parse(const char *s, size_t len, json_tok_t *t, int max);

/* 객체 obj 에서 key 의 값 토큰 번호. 없으면 -1. */
int json_get(const char *s, const json_tok_t *t, int obj, const char *key);

/* 문자열 토큰을 풀어(이스케이프 처리, UTF-8) out 에 NUL 로 끝나게 담는다. 길이를
 * 돌려준다. 문자열이 아니거나, 자리가 모자라거나, 풀린 값에 NUL 이 있으면 -1. */
int json_string(const char *s, const json_tok_t *t, int i, char *out, size_t cap);

/* 문자열 토큰이 이스케이프 없이 want 와 같으면 1. */
int json_eq(const char *s, const json_tok_t *t, int i, const char *want);

/* 정수인 숫자 토큰. 소수점이나 지수가 있으면 정수 부분만 쓴다 (JWT 의 NumericDate 는
 * 소수를 허용한다). 숫자가 아니거나 범위를 넘으면 -1. */
int json_int64(const char *s, const json_tok_t *t, int i, int64_t *out);

#endif
