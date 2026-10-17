#pragma once

#ifndef __AFXWIN_H__
#error "include framework.h before DiagDlg.h"
#endif

#include <deque>

#include "PacketStore.h"
#include "ui_theme.h"
#include "ui_controls.h"

/* 텍스트 로그가 무한히 쌓이지 않게 자른다. */
#define LOG_MAX_LINES 2000

#define DIAG_TIMER_ID 1
#define DIAG_TIMER_MS 500

/* 탭 순서 */
enum {
	DIAG_TAB_PACKETS = 0,
	DIAG_TAB_LOG
};

/* 통계 타일 */
enum {
	TILE_TX = 0,
	TILE_RX,
	TILE_KEEPALIVE,
	TILE_DROPS,
	TILE_REHANDSHAKE,
	TILE_COUNT
};

/* 진단 창: 터널을 오가는 패킷, 텍스트 로그, 통계.
 *
 * 쓰는 사람이 평소에 볼 것이 아니다. 주 창은 "연결됐는가, 왜 막혔는가"만 보이고, 이 창은
 * 그 아래에서 무슨 일이 있었는지를 개발자나 지원 담당자가 볼 때 연다. 상용 에이전트가
 * 진단 / 로그 창을 따로 두는 것과 같다.
 *
 * 데이터는 주 창이 갖는다. 서비스에서 오는 메시지는 주 창이 받고, 패킷과 로그를 쌓은
 * 뒤 이 창이 열려 있으면 알려 준다. 그래서 이 창을 닫아 둔 동안의 일도 열면 보인다.
 * 닫기는 숨기기다 — 다시 열 때 필터와 스크롤 위치가 그대로다. */
class CDiagDlg : public CDialogEx
{
public:
	CDiagDlg(CPacketStore &store, std::deque<CString> &log);

	/* 처음이면 만들고, 보이게 하고 앞으로 가져온다. */
	void Open(CWnd *owner);
	bool IsOpen() const;

	/* 주 창이 부른다. 창이 없으면 아무것도 하지 않는다. */
	void PacketsChanged();
	void LogAppended(const CString &line);

protected:
	BOOL OnInitDialog() override;
	void DoDataExchange(CDataExchange *pDX) override;
	void OnCancel() override;
	void OnOK() override;

	afx_msg void OnClear();
	afx_msg void OnSave();
	afx_msg void OnFilterChanged();
	afx_msg void OnTabChanged();
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO *mmi);
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg HBRUSH OnCtlColor(CDC *pDC, CWnd *pWnd, UINT type);
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg LRESULT OnPrintClient(WPARAM wParam, LPARAM lParam);
	afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT dis);
	afx_msg void OnSysColorChange();

	/* 가상 리스트: 행 내용을 컨트롤에 복사해 두지 않고 그릴 때마다 물어본다. */
	afx_msg void OnGetDispInfo(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnPacketCustomDraw(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnPacketSelChanged(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnGetEmptyMarkup(NMHDR *pNMHDR, LRESULT *pResult);

	DECLARE_MESSAGE_MAP()

private:
	void StyleList(CListCtrl &list);
	void SetupPacketColumns();
	void ApplyFilter();
	void RefreshPacketCount();
	void UpdateStats();
	void ShowDetail(int viewIndex);
	void AddLogLine(const CString &line);
	void PaintBackground(HDC dc);
	void PaintTile(HDC dc, int tile);
	void LayoutChildren(int cx, int cy);

	CPacketStore &m_store;
	std::deque<CString> &m_logLines;

	CListCtrl m_packets;
	CListBox m_log;
	CEdit m_detail;
	HWND m_tabs;			/* ui_controls 의 탭 줄 */

	ui::Fonts m_fonts;
	CBrush m_card_brush;
	CImageList m_row_pad;		/* 리스트의 행 높이를 늘리는 빈 이미지 목록 */
	CRect m_rc_tiles[TILE_COUNT];
	CRect m_rc_content;
	CRect m_rc_detail;
	CRect m_rc_footer;
	int m_sep_y;			/* 탭 줄 아래 구분선 */

	CString m_tile_value[TILE_COUNT];
	CString m_tile_note[TILE_COUNT];
	CString m_footer;
};
