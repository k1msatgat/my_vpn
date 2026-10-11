#include <stdio.h>
#include <string.h>
#include <sys/random.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "auth.h"

int auth_hmac(const uint8_t key[AUTH_KEY_LEN], const uint8_t *msg, size_t len,
		uint8_t mac[AUTH_MAC_LEN])
{
	unsigned int out_len = 0;

	if (HMAC(EVP_sha256(), key, AUTH_KEY_LEN, msg, len, mac, &out_len) == NULL) {
		return -1;
	}

	return out_len == AUTH_MAC_LEN ? 0 : -1;
}

/* memcmp 는 처음 다른 바이트에서 멈춘다. 그러면 응답까지 걸린 시간이 "앞에서 몇
 * 바이트가 맞았는가"를 알려 주고, 공격자는 한 바이트씩 맞춰 가며 올바른 mac 을
 * 만들어 낼 수 있다. CRYPTO_memcmp 는 끝까지 다 보고 나서 결과를 낸다. */
int auth_mac_equal(const uint8_t a[AUTH_MAC_LEN], const uint8_t b[AUTH_MAC_LEN])
{
	return CRYPTO_memcmp(a, b, AUTH_MAC_LEN) == 0;
}

/* nonce 가 예측되면 공격자가 미리 서명을 받아 둘 수 있으므로 rand() 는 쓸 수 없다.
 * 세션 idx 를 만들 때와 같이 커널의 getrandom 을 쓴다. 요청한 길이가 256 바이트
 * 이하이면 한 번에 다 채워 주므로 부분 읽기를 따로 처리하지 않는다. */
int auth_random(uint8_t *buf, size_t len)
{
	return getrandom(buf, len, 0) == (ssize_t)len ? 0 : -1;
}

int auth_selftest(void)
{
	/* auth_hmac 은 키 길이가 AUTH_KEY_LEN 으로 고정이라, RFC 의 짧은 키는 뒤를 0 으로
	 * 채워 넣는다. HMAC 은 블록(64B)보다 짧은 키를 0 으로 채워 쓰므로 결과가 같다. */
	static const struct {
		const char *name;
		uint8_t key[AUTH_KEY_LEN];
		const char *msg;
		uint8_t want[AUTH_MAC_LEN];
	} cases[] = {
		{ "rfc4231 case 1",
		  { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b },
		  "Hi There",
		  { 0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce,
		    0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
		    0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7 } },
		{ "rfc4231 case 2",
		  { 'J', 'e', 'f', 'e' },
		  "what do ya want for nothing?",
		  { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26,
		    0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
		    0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 } },
	};
	uint8_t got[AUTH_MAC_LEN];
	int failed = 0;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int ok = auth_hmac(cases[i].key, (const uint8_t *)cases[i].msg,
				strlen(cases[i].msg), got) == 0 &&
			auth_mac_equal(got, cases[i].want);

		printf("hmac-sha256 %-16s %s\n", cases[i].name, ok ? "ok" : "FAIL");
		failed |= !ok;
	}

	return failed;
}
