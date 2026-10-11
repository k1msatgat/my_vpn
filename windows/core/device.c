#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <string.h>

#include "device.h"

/* GUI 의 연결 설정(HKCU\Software\my_vpn\vpn_gui)과 같은 뿌리 아래에 둔다.
 * 콘솔 클라이언트와 GUI 가 같은 신원을 쓰도록 프로그램 이름 아래가 아니라 옆에 둔다. */
#define DEVICE_REG_PATH	L"Software\\my_vpn\\device"
#define DEVICE_REG_ID	L"id"
#define DEVICE_REG_KEY	L"key"

/* 키를 레지스트리에 그대로 쓰면 그 하이브를 읽을 수 있는 누구나(백업 파일, 다른
 * 계정의 관리자, 디스크를 떼어 간 사람) 키를 얻는다. DPAPI(CryptProtectData)는
 * 현재 사용자의 로그온 자격 증명에서 유도한 키로 암호화하므로, 저장된 값은 같은
 * PC 의 같은 사용자로 로그온해야만 풀린다. 키 관리(어디에 암호화 키를 둘 것인가)를
 * OS 에 맡기는 것이다.
 *
 * 막지 못하는 것: 같은 사용자로 도는 다른 프로세스는 똑같이 CryptUnprotectData 를
 * 부를 수 있다. 그것까지 막으려면 TPM 에 키를 두고 꺼낼 수 없게 해야 한다. */
static int key_protect(const uint8_t *key, DATA_BLOB *sealed)
{
	DATA_BLOB plain;

	plain.pbData = (BYTE *)key;
	plain.cbData = AUTH_KEY_LEN;

	/* 이 프로그램은 서비스나 워커 스레드에서도 돌 수 있어 UI 를 띄우면 안 된다. */
	if (!CryptProtectData(&plain, L"my_vpn device key", NULL, NULL, NULL,
			CRYPTPROTECT_UI_FORBIDDEN, sealed)) {
		return -1;
	}

	return 0;
}

static int key_unprotect(const BYTE *sealed, DWORD sealed_len, uint8_t *key)
{
	DATA_BLOB in;
	DATA_BLOB plain;
	int ret = -1;

	in.pbData = (BYTE *)sealed;
	in.cbData = sealed_len;

	if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
			CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
		return -1;
	}

	if (plain.cbData == AUTH_KEY_LEN) {
		memcpy(key, plain.pbData, AUTH_KEY_LEN);
		ret = 0;
	}

	/* 평문 키가 해제된 힙에 남지 않게 지우고 돌려준다. 컴파일러가 "곧 해제될
	 * 메모리"에 대한 memset 을 최적화로 없앨 수 있어 SecureZeroMemory 를 쓴다. */
	SecureZeroMemory(plain.pbData, plain.cbData);
	LocalFree(plain.pbData);

	return ret;
}

int device_load(device_t *out)
{
	BYTE sealed[1024];
	DWORD len;
	LSTATUS err;

	memset(out, 0, sizeof(*out));

	len = sizeof(out->id);
	err = RegGetValueW(HKEY_CURRENT_USER, DEVICE_REG_PATH, DEVICE_REG_ID,
			RRF_RT_REG_BINARY, NULL, out->id, &len);
	if (err == ERROR_FILE_NOT_FOUND) {
		return DEVICE_NOT_ENROLLED;
	}
	if (err != ERROR_SUCCESS || len != sizeof(out->id)) {
		return DEVICE_ERROR;
	}

	len = sizeof(sealed);
	err = RegGetValueW(HKEY_CURRENT_USER, DEVICE_REG_PATH, DEVICE_REG_KEY,
			RRF_RT_REG_BINARY, NULL, sealed, &len);
	if (err != ERROR_SUCCESS) {
		return DEVICE_ERROR;
	}

	if (key_unprotect(sealed, len, out->key) < 0) {
		return DEVICE_ERROR;
	}

	return DEVICE_OK;
}

int device_enroll(device_t *out)
{
	DATA_BLOB sealed = { 0 };
	HKEY reg = NULL;
	int ret = DEVICE_ERROR;

	/* rand() 는 쓸 수 없다. 시드와 출력 몇 개로 다음 값을 맞힐 수 있는 생성기라
	 * 키가 예측된다. OS 의 암호학적 난수 생성기를 쓴다. */
	if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)out, sizeof(*out),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
		return DEVICE_ERROR;
	}

	if (key_protect(out->key, &sealed) < 0) {
		goto out;
	}

	if (RegCreateKeyExW(HKEY_CURRENT_USER, DEVICE_REG_PATH, 0, NULL, 0,
			KEY_SET_VALUE, NULL, &reg, NULL) != ERROR_SUCCESS) {
		goto out;
	}

	if (RegSetValueExW(reg, DEVICE_REG_ID, 0, REG_BINARY,
				out->id, sizeof(out->id)) != ERROR_SUCCESS ||
			RegSetValueExW(reg, DEVICE_REG_KEY, 0, REG_BINARY,
				sealed.pbData, sealed.cbData) != ERROR_SUCCESS) {
		goto out;
	}

	ret = DEVICE_OK;

out:
	if (reg) {
		RegCloseKey(reg);
	}
	if (sealed.pbData) {
		LocalFree(sealed.pbData);
	}
	if (ret != DEVICE_OK) {
		SecureZeroMemory(out, sizeof(*out));
	}

	return ret;
}
