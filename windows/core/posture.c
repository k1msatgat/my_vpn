#include <windows.h>
#include <objbase.h>
#include <netfw.h>
#include <string.h>

#include "posture.h"

/* ---- OS version ---------------------------------------------------------- */
/* GetVersionExW 는 쓸 수 없다. Windows 8.1 부터 실행 파일의 매니페스트에 적힌
 * 호환성 선언을 보고 값을 돌려주므로, 선언이 없으면 Windows 11 에서도 6.2
 * (Windows 8)라고 답한다. 포스처는 프로그램이 "어떤 OS 로 취급받는가"가 아니라
 * 실제로 깔린 OS 를 보고해야 하므로, 그 보정을 거치지 않는 ntdll 의
 * RtlGetVersion 을 부른다.
 *
 * ntdll.dll 은 모든 프로세스에 이미 올라와 있어 LoadLibrary 가 아니라
 * GetModuleHandle 로 얻는다. 참조 횟수를 올리지 않으므로 FreeLibrary 도 없다. */
typedef LONG (WINAPI *rtl_get_version_fn)(RTL_OSVERSIONINFOW *info);

static void collect_os(posture_t *out)
{
	RTL_OSVERSIONINFOW info;
	rtl_get_version_fn rtl_get_version;
	HMODULE ntdll;

	ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) {
		return;
	}

	rtl_get_version = (rtl_get_version_fn)GetProcAddress(ntdll, "RtlGetVersion");
	if (!rtl_get_version) {
		return;
	}

	memset(&info, 0, sizeof(info));
	info.dwOSVersionInfoSize = sizeof(info);
	if (rtl_get_version(&info) != 0) { /* STATUS_SUCCESS == 0 */
		return;
	}

	out->os_major = info.dwMajorVersion;
	out->os_minor = info.dwMinorVersion;
	out->os_build = info.dwBuildNumber;
}

/* ---- firewall ------------------------------------------------------------ */
/* Windows 방화벽 설정은 COM 인터페이스 INetFwPolicy2 로 읽는다. 레지스트리를 직접
 * 읽는 방법도 있지만 그룹 정책이 덮어쓴 값은 다른 키에 있어서, 둘을 합친 "실제로
 * 적용되는 값"은 이 인터페이스가 계산해 준다.
 *
 * 방화벽은 프로필(도메인 / 개인 / 공용)마다 따로 켜고 끈다. 지금 연결된 네트워크에
 * 걸린 프로필만 의미가 있으므로 get_CurrentProfileTypes 로 활성 프로필을 얻고,
 * 그중 하나라도 꺼져 있으면 OFF 로 본다. 네트워크가 여러 개면 프로필도 여러 개가
 * 동시에 활성일 수 있어 비트마스크로 온다.
 *
 * C 에서는 COM 메서드를 obj->lpVtbl->Method(obj, ...) 로 부른다. C++ 의
 * obj->Method(...) 가 실제로 하는 일(가상 함수 테이블에서 함수 포인터를 꺼내
 * this 를 첫 인자로 넘김)을 손으로 쓰는 것이다. */
static uint8_t collect_firewall(void)
{
	static const NET_FW_PROFILE_TYPE2 PROFILES[] = {
		NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC
	};
	INetFwPolicy2 *policy = NULL;
	uint8_t state = POSTURE_UNKNOWN;
	long active = 0;
	HRESULT init;
	HRESULT hr;
	size_t i;

	/* 호출한 스레드가 COM 을 이미 초기화했을 수 있다.
	 *   S_OK / S_FALSE      우리 호출이 참조를 하나 올렸으므로 CoUninitialize 로 짝을 맞춘다.
	 *   RPC_E_CHANGED_MODE  다른 모드로 이미 초기화돼 있다. 그대로 쓸 수 있고, 우리가
	 *                       올린 참조가 없으니 CoUninitialize 를 부르면 안 된다. */
	init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
		return POSTURE_UNKNOWN;
	}

	hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER,
			&IID_INetFwPolicy2, (void **)&policy);
	if (FAILED(hr)) {
		goto out;
	}

	hr = policy->lpVtbl->get_CurrentProfileTypes(policy, &active);
	if (FAILED(hr) || active == 0) {
		goto out;
	}

	state = POSTURE_ON;
	for (i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
		VARIANT_BOOL enabled = VARIANT_FALSE;

		if (!(active & PROFILES[i])) {
			continue;
		}

		hr = policy->lpVtbl->get_FirewallEnabled(policy, PROFILES[i], &enabled);
		if (FAILED(hr)) {
			state = POSTURE_UNKNOWN;
			break;
		}
		if (enabled == VARIANT_FALSE) {
			state = POSTURE_OFF;
			break;
		}
	}

out:
	if (policy) {
		policy->lpVtbl->Release(policy);
	}
	if (SUCCEEDED(init)) {
		CoUninitialize();
	}

	return state;
}

void posture_collect(posture_t *out)
{
	memset(out, 0, sizeof(*out));

	collect_os(out);
	out->firewall = collect_firewall();
}
