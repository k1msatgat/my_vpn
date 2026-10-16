#include "framework.h"
#include "VpnGuiApp.h"
#include "MainDlg.h"
#include "ui_theme.h"
#include "ui_controls.h"

CVpnGuiApp theApp;

CVpnGuiApp::CVpnGuiApp()
	: m_wsa_ready(false)
{
}

BOOL CVpnGuiApp::InitInstance()
{
	INITCOMMONCONTROLSEX icc;
	WSADATA wsa;

	ZeroMemory(&icc, sizeof(icc));
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icc);

	if (!CWinApp::InitInstance()) {
		return FALSE;
	}

	/* 연결 설정을 다음 실행에도 기억하도록 레지스트리 키를 정한다
	 * (HKCU\Software\ztgate\vpn_gui). 지정하지 않으면 MFC 가 .ini 를 쓴다. */
	SetRegistryKey(_T("ztgate"));

	/* UI 스레드도 InetPtonW 를 쓰므로 Winsock 이 필요하다. tunnel_run 안에서도
	 * WSAStartup / WSACleanup 을 한 번 더 하지만, Winsock 은 호출 횟수를 세므로
	 * 중첩해도 문제가 없다. */
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		AfxMessageBox(_T("WSAStartup 실패"), MB_ICONERROR);
		return FALSE;
	}
	m_wsa_ready = true;

	/* 색과 GDI+ 를 준비하고, .rc 가 탭 줄 컨트롤을 만들 수 있게 창 클래스를 등록한다.
	 * 둘 다 다이얼로그가 만들어지기 전이어야 한다 — 등록되지 않은 클래스의 컨트롤이
	 * 템플릿에 있으면 다이얼로그 생성이 통째로 실패한다. */
	ui::Startup();
	if (!ui::RegisterTabs(AfxGetInstanceHandle())) {
		AfxMessageBox(_T("창 클래스를 등록할 수 없습니다."), MB_ICONERROR);
		return FALSE;
	}

	CMainDlg dlg;
	m_pMainWnd = &dlg;
	dlg.DoModal();
	m_pMainWnd = nullptr;

	/* 모달 대화상자로 끝나는 앱이라 메시지 루프에 들어가지 않고 바로 종료한다. */
	return FALSE;
}

int CVpnGuiApp::ExitInstance()
{
	if (m_wsa_ready) {
		WSACleanup();
		m_wsa_ready = false;
	}
	ui::Shutdown();

	return CWinApp::ExitInstance();
}
