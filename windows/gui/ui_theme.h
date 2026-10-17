#pragma once

/* 화면의 색과 글꼴, 그리는 도구. MFC 를 쓰지 않는 순수 Win32 코드다.
 *
 * 다이얼로그 편집기가 주는 기본 모양(회색 바탕, 그룹 상자, 입체 버튼)을 쓰지 않고
 * 바탕과 "카드"를 직접 그린다. 그러려면 색이 한 곳에 있어야 한다 — 버튼, 탭, 리스트,
 * 다이얼로그 바탕이 저마다 RGB 를 적으면 하나를 고칠 때 나머지가 어긋난다.
 *
 * 고대비 모드에서는 우리 색을 버리고 시스템 색을 쓴다. 고대비를 켠 사용자는 자기가
 * 고른 색 조합이 아니면 글자를 읽지 못할 수 있다. */

#include <windows.h>

namespace ui {

struct Palette {
	COLORREF bg;		/* 창 바탕 */
	COLORREF card;		/* 컨트롤이 놓이는 흰 판 */
	COLORREF border;	/* 판의 테두리, 구분선 */
	COLORREF text;
	COLORREF muted;		/* 라벨, 보조 설명 */

	COLORREF accent;	/* 주 버튼, 선택된 탭 */
	COLORREF accentHover;
	COLORREF accentPressed;
	COLORREF onAccent;	/* accent 위의 글자 */

	COLORREF button;	/* 보조 버튼 */
	COLORREF buttonHover;
	COLORREF buttonPressed;
	COLORREF buttonBorder;
	COLORREF disabledText;
	COLORREF disabledFill;

	COLORREF ok;		/* 연결됨 */
	COLORREF warn;		/* 연결 중 */
	COLORREF danger;	/* 거부, 폐기 */
	COLORREF dangerBg;	/* 폐기된 행의 바탕 */
	COLORREF idle;		/* 연결 안 됨 */
	COLORREF tx;		/* 송신 */
	COLORREF rx;		/* 수신 */

	bool highContrast;
};

const Palette &Colors();

/* GDI+ 를 켜고 색을 정한다. 프로세스에서 한 번, 창을 만들기 전에. */
void Startup();
void Shutdown();

/* 시스템 색이나 고대비 설정이 바뀌었을 때 (WM_SYSCOLORCHANGE). */
void RefreshPalette();

/* 다이얼로그 글꼴에서 파생한 글꼴들. 크기를 포인트로 다시 적지 않고 다이얼로그
 * 글꼴의 실제 높이에 비율을 곱한다 — 그 글꼴은 이미 시스템 DPI 에 맞춰져 있다. */
struct Fonts {
	HFONT base;	/* 다이얼로그 글꼴 그대로 (소유하지 않는다) */
	HFONT label;	/* 라벨 */
	HFONT strong;	/* 같은 크기, 세미볼드 */
	HFONT value;	/* 통계 타일의 숫자 */
	HFONT title;	/* 연결 상태 */
	HFONT mono;	/* 상세창, 로그, 16진수 */
	HFONT hero;	/* 주 창 한가운데의 상태 */
	HFONT icon;	/* 상태 원 안의 기호 (Segoe MDL2 Assets) */
	HFONT iconSmall;	/* 목록 앞의 작은 기호 */
};

/* Segoe MDL2 Assets 의 기호. Windows 10 부터 늘 있는 글꼴이라 그림 파일을 따로 두지 않는다. */
#define ICON_CHECK	L"\xE73E"
#define ICON_CROSS	L"\xE711"
#define ICON_SYNC	L"\xE895"
#define ICON_POWER	L"\xE7E8"
#define ICON_WARNING	L"\xE7BA"
#define ICON_UNKNOWN	L"\xE9CE"

void CreateFonts(HFONT dialogFont, Fonts *out);
void DestroyFonts(Fonts *fonts);

/* 96 DPI 기준 픽셀을 지금 화면의 픽셀로. */
int Scale(int px);

/* 모서리가 둥근 사각형. fill 이나 border 에 CLR_NONE 을 주면 그쪽은 그리지 않는다. */
void RoundRect(HDC dc, const RECT &rc, int radius, COLORREF fill, COLORREF border);
void Dot(HDC dc, int cx, int cy, int radius, COLORREF color);

/* format 은 DrawText 의 것 (DT_LEFT | DT_VCENTER | DT_SINGLELINE ...). */
void Text(HDC dc, const RECT &rc, LPCWSTR text, HFONT font, COLORREF color, UINT format);
SIZE Measure(HDC dc, LPCWSTR text, HFONT font);

/* 제목 표시줄을 창 바탕과 같은 색으로 (Windows 11). 예전 버전에서는 아무 일도 없다. */
void MatchTitleBar(HWND hwnd);

} /* namespace ui */
