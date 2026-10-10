#include "framework.h"
#include "VpnGuiApp.h"
#include "MainDlg.h"

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

	/* UI 스레드도 InetPtonW 를 쓰므로 Winsock 이 필요하다. tunnel_run 안에서도
	 * WSAStartup / WSACleanup 을 한 번 더 하지만, Winsock 은 호출 횟수를 세므로
	 * 중첩해도 문제가 없다. */
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		AfxMessageBox(_T("WSAStartup 실패"), MB_ICONERROR);
		return FALSE;
	}
	m_wsa_ready = true;

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

	return CWinApp::ExitInstance();
}
