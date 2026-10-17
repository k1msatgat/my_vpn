#pragma once

/* framework.h 를 먼저 포함해야 한다 (Winsock2 -> MFC 순서). */
#ifndef __AFXWIN_H__
#error "include framework.h before MainDlg.h"
#endif

#include "vpn_remote.h"
#include "posture.h"
#include <deque>

#include "PacketStore.h"
#include "DiagDlg.h"
#include "ui_theme.h"
#include "ui_controls.h"

/* 한 번에 링버퍼에서 꺼내는 패킷 수. 너무 크면 UI 가 한 번에 오래 멈춘다. */
#define PACKET_DRAIN_BATCH 256

#define TIMER_TICK_ID 1
#define TIMER_TICK_MS 500

/* 포스처 탭이 이 PC 의 상태를 다시 읽는 주기 (틱 수). 코어의 보고 주기와 같은 2초. */
#define POSTURE_REFRESH_TICKS 4

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
	afx_msg void OnEnroll();
	afx_msg void OnDiag();
	afx_msg void OnSettings();
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO *mmi);
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg HBRUSH OnCtlColor(CDC *pDC, CWnd *pWnd, UINT type);

	/* 바탕, 판, 상태, 통계는 컨트롤이 아니라 다이얼로그가 직접 그린다. */
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg LRESULT OnPrintClient(WPARAM wParam, LPARAM lParam);
	afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT dis);
	afx_msg void OnSysColorChange();

	afx_msg void OnPostureCustomDraw(NMHDR *pNMHDR, LRESULT *pResult);

	/* 워커 스레드가 PostMessage 로 보낸 것들. 모두 UI 스레드에서 처리된다. */
	afx_msg LRESULT OnVpnLog(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnState(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnDone(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnPackets(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnService(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnDevice(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnVpnError(WPARAM wParam, LPARAM lParam);
	bool ServiceReady();

	DECLARE_MESSAGE_MAP()

private:
	bool BuildConfig(tunnel_config_t &cfg);
	void AppendLog(const CString &text);
	void RequestDisconnect();
	bool EditSettings(int bad_id, const CString &why);
	bool SettingsUsable();
	void UpdateInfo();
	void SetUiState(tunnel_state_t state);
	void UpdateStatus();
	void PaintBackground(HDC dc);
	void StyleList(CListCtrl &list);
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

	uint32_t m_session_idx;		/* 지금 세션, 없으면 0 */
	CString m_tun_text;		/* 서버가 배정한 터널 주소. 같은 값으로 다시 그리지 않으려고 기억한다 */

	/* 연결 설정 (레지스트리에 둔다). 설정 창에서만 바뀐다. */
	CString m_server;
	CString m_port;
	CString m_adapter;
	int m_link;			/* 서비스와의 연결 (vpn_remote_link_t) */
	bool m_stopping;		/* 해제를 요청하고 워커가 끝나기를 기다리는 중 */

	CListCtrl m_posture;

	/* 패킷과 로그는 진단 창이 닫혀 있어도 여기에 쌓는다. 진단 창은 이것을 보여 줄 뿐이다. */
	std::deque<CString> m_logLines;
	CDiagDlg m_diag;

	/* 직접 그리는 것들. 자리는 LayoutChildren 이 정하고 PaintBackground 가 그린다. */
	ui::Fonts m_fonts;
	CBrush m_card_brush;		/* 판 위에 놓인 라벨 / 체크 상자의 바탕 */
	CImageList m_row_pad;		/* 리스트의 행 높이를 늘리는 빈 이미지 목록 */
	CRect m_rc_header;
	CRect m_rc_status;
	CRect m_rc_info;		/* 머리 가운데의 연결 정보 */		/* 상태 글자가 바뀔 때 다시 그릴 영역 */
	CRect m_rc_content;
	CRect m_rc_content_title;
	int m_sep_y;			/* 제목 줄 아래 구분선 */

	CString m_info_server;
	CString m_info_tunnel;
	CString m_status_title;
	CString m_status_sub;
	COLORREF m_status_dot;
	COLORREF m_status_sub_color;
};
