#include <windows.h>
#include <bcrypt.h>

#include "hmac.h"

/* 암호 알고리즘은 직접 구현하지 않고 Windows 에 내장된 CNG(BCrypt)를 쓴다.
 * 직접 짠 SHA-256 은 테스트 벡터를 통과해도 타이밍 같은 부채널까지 검증하기
 * 어렵고, OS 구현은 보안 패치와 함께 갱신된다. 서버(Linux)는 OpenSSL 을 쓰지만
 * HMAC-SHA256 은 표준(RFC 2104)이라 같은 입력에 같은 값이 나온다.
 *
 * CNG 의 호출 순서:
 *   알고리즘 공급자 열기  ->  해시 객체 만들기(키를 여기서 준다)
 *   ->  데이터 넣기  ->  결과 꺼내기  ->  역순으로 닫기
 *
 * BCRYPT_ALG_HANDLE_HMAC_FLAG 를 줘야 SHA-256 이 아니라 HMAC-SHA256 이 된다. */
int hmac_sha256(const uint8_t *key, size_t key_len,
		const uint8_t *msg, size_t msg_len,
		uint8_t out[HMAC_SHA256_LEN])
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	int ret = -1;

	if (key_len > MAXDWORD || msg_len > MAXDWORD) {
		return -1;
	}

	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
			NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
		return -1;
	}

	/* 해시 객체 버퍼를 NULL / 0 으로 주면 CNG 가 내부에서 할당한다. */
	if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, NULL, 0,
			(PUCHAR)key, (ULONG)key_len, 0))) {
		goto out;
	}

	if (!BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)msg, (ULONG)msg_len, 0))) {
		goto out;
	}

	if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, out, HMAC_SHA256_LEN, 0))) {
		goto out;
	}

	ret = 0;

out:
	if (hash) {
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(alg, 0);

	return ret;
}
