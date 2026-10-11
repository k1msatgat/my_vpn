#include <windows.h>
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

void posture_collect(posture_t *out)
{
	memset(out, 0, sizeof(*out));

	collect_os(out);
}
