#pragma once

/* framework.h 를 먼저 포함해야 한다 (Winsock2 -> MFC 순서). */
#ifndef __AFXWIN_H__
#error "include framework.h before MainDlg.h"
#endif

#include "vpn_session.h"
#include "posture.h"
#include "PacketStore.h"

/* 텍스트 로그가 무한히 쌓이지 않게 자른다. */
#define LOG_MAX_LINES 2000

/* 한 번에 링버퍼에서 꺼내는 패킷 수. 너무 크면 UI 가 한 번에 오래 멈춘다. */
#define PACKET_DRAIN_BATCH 256

#define TIMER_TICK_ID 1
#define TIMER_TICK_MS 500

/* 포스처 탭이 이 PC 의 상태를 다시 읽는 주기 (틱 수). 코어의 보고 주기와 같은 2초. */
#define POSTURE_REFRESH_TICKS 4

/* 탭 순서 */
enum {
	TAB_PACKETS = 0,
	TAB_LOG,
	TAB_POSTURE
};

/* 포스처 탭의 행 */
enum {
	POSTURE_ROW_DEVICE = 0,
	POSTURE_ROW_OS,
	POSTURE_ROW_FIREWALL,
	POSTURE_ROW_ANTIVIRUS,
	POSTURE_ROW_VERDICT,
	POSTURE_ROW_COUNT
};

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
	afx_msg void OnEnroll();
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
	afx_msg void OnPostureCustomDraw(NMHDR *pNMHDR, LRESULT *pResult);

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

	/* 포스처 탭 */
	void SetupPostureList();
	void LoadDeviceId();
	void TrackAuth(const tunnel_packet_t &p);
	void RefreshPosture(bool collect);
	void SetPostureCell(int row, int col, const CString &text);
	CString ServerVerdictText() const;
	static CString LocalStateName(int state);

	/* 표시 문자열은 UI 가 만든다 — 코어는 enum 만 올린다. */
	CString ColumnText(const tunnel_packet_t &p, int col) const;
	static CString DirName(const tunnel_packet_t &p);
	static CString TypeName(const tunnel_packet_t &p);
	static CString VerdictName(int verdict);
	static CString PostureStateName(int state);
	static CString DenyReasonName(int reason);
	static CString PostureText(const posture_report_t &r);
	static CString Summary(const tunnel_packet_t &p);
	static CString ProtoName(const tunnel_packet_t &p);
	static CString TimeText(ULONGLONG t_ms);

	vpn_session_t *m_session;
	CPacketStore m_store;
	tunnel_state_t m_state;
	ULONGLONG m_connected_at;	/* GetTickCount64, 0 이면 미연결 */

	/* 포스처 탭에 보여 줄 것들.
	 *   m_local     이 PC 의 지금 상태. UI 가 직접 읽는다 (연결하지 않아도 보인다).
	 *   m_reported  서버에 마지막으로 보고한 값. 패킷 이벤트에서 얻는다.
	 * 둘을 나란히 보여 주면 "바뀌었는데 아직 보고되지 않은" 순간이 보인다. */
	posture_t m_local;
	posture_report_t m_reported;
	bool m_has_reported;
	int m_deny_reason;		/* 마지막 거부 사유, 없으면 0 */
	CString m_device_id;		/* 비어 있으면 미등록 */
	bool m_posture_bad[POSTURE_ROW_COUNT];
	int m_posture_ticks;

	CListCtrl m_packets;
	CListCtrl m_posture;
	CListBox m_log;
	CTabCtrl m_tab;
	CEdit m_detail;
	CFont m_mono;			/* 상세창 / hex 용 고정폭 */
	COLORREF m_state_color;
};
