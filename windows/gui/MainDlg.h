#pragma once

/* framework.h 를 먼저 포함해야 한다 (Winsock2 -> MFC 순서). */
#ifndef __AFXWIN_H__
#error "include framework.h before MainDlg.h"
#endif

#include "vpn_session.h"

/* 로그가 무한히 쌓이지 않게 자른다. 패킷 로그를 켜면 금방 수만 줄이 된다. */
#define LOG_MAX_LINES 2000

class CMainDlg : public CDialogEx
{
public:
	explicit CMainDlg(CWnd *parent = nullptr);

protected:
	BOOL OnInitDialog() override;
	void DoDataExchange(CDataExchange *pDX) override;

	afx_msg void OnConnect();
	afx_msg void OnDisconnect();
	afx_msg void OnDestroy();

	/* 워커 스레드가 PostMessage 로 보낸 것들. 모두 UI 스레드에서 처리된다. */
	afx_msg LRESULT OnVpnLog(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnState(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnDone(WPARAM wParam, LPARAM lParam);

	DECLARE_MESSAGE_MAP()

private:
	bool BuildConfig(tunnel_config_t &cfg);
	void AppendLog(const CString &text);
	void SetUiState(tunnel_state_t state);

	vpn_session_t *m_session;
	CListBox m_log;
};
