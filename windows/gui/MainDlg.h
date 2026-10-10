#pragma once

/* framework.h 를 먼저 포함해야 한다 (Winsock2 -> MFC 순서). */
#ifndef __AFXWIN_H__
#error "include framework.h before MainDlg.h"
#endif

#include "vpn_session.h"
#include "PacketStore.h"

/* 텍스트 로그가 무한히 쌓이지 않게 자른다. */
#define LOG_MAX_LINES 2000

/* 한 번에 링버퍼에서 꺼내는 패킷 수. 너무 크면 UI 가 한 번에 오래 멈춘다. */
#define PACKET_DRAIN_BATCH 256

#define TIMER_TICK_ID 1
#define TIMER_TICK_MS 500

class CMainDlg : public CDialogEx
{
public:
	explicit CMainDlg(CWnd *parent = nullptr);

protected:
	BOOL OnInitDialog() override;
	void DoDataExchange(CDataExchange *pDX) override;

	afx_msg void OnConnect();
	afx_msg void OnDisconnect();
	afx_msg void OnClear();
	afx_msg void OnSave();
	afx_msg void OnFilterChanged();
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO *mmi);
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg HBRUSH OnCtlColor(CDC *pDC, CWnd *pWnd, UINT type);

	/* 가상 리스트: 행 내용을 컨트롤에 복사해 두지 않고 그릴 때마다 물어본다. */
	afx_msg void OnGetDispInfo(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnPacketCustomDraw(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnPacketSelChanged(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnTabChanged(NMHDR *pNMHDR, LRESULT *pResult);

	/* 워커 스레드가 PostMessage 로 보낸 것들. 모두 UI 스레드에서 처리된다. */
	afx_msg LRESULT OnVpnLog(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnState(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnDone(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnPackets(WPARAM wParam, LPARAM lParam);

	DECLARE_MESSAGE_MAP()

private:
	bool BuildConfig(tunnel_config_t &cfg);
	void AppendLog(const CString &text);
	void SetUiState(tunnel_state_t state);
	void SetupPacketColumns();
	void ApplyFilter();
	void RefreshPacketCount();
	void UpdateStats();
	void ShowDetail(int viewIndex);
	void LayoutChildren(int cx, int cy);
	void LoadSettings();
	void SaveSettings();

	/* 표시 문자열은 UI 가 만든다 — 코어는 enum 만 올린다. */
	CString ColumnText(const tunnel_packet_t &p, int col) const;
	static CString DirName(const tunnel_packet_t &p);
	static CString TypeName(const tunnel_packet_t &p);
	static CString VerdictName(int verdict);
	static CString Summary(const tunnel_packet_t &p);
	static CString ProtoName(const tunnel_packet_t &p);
	static CString TimeText(ULONGLONG t_ms);

	vpn_session_t *m_session;
	CPacketStore m_store;
	tunnel_state_t m_state;
	ULONGLONG m_connected_at;	/* GetTickCount64, 0 이면 미연결 */

	CListCtrl m_packets;
	CListBox m_log;
	CTabCtrl m_tab;
	CEdit m_detail;
	CFont m_mono;			/* 상세창 / hex 용 고정폭 */
	COLORREF m_state_color;
};
