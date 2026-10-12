#pragma once

#ifndef __AFXWIN_H__
#error "include framework.h before EnrollDlg.h"
#endif

#include "device.h"
#include "ui_theme.h"
#include "ui_controls.h"

/* 방금 만든 기기 ID 와 키를 한 번 보여 주는 창.
 *
 * 키는 등록 직후 이 창에서만 볼 수 있다. 이후에는 DPAPI 로 감싼 값만 남고 프로그램도
 * 다시 보여 주지 않는다. 관리자가 이 값을 서버의 기기 목록에 옮겨 적는다. */
class CEnrollDlg : public CDialogEx
{
public:
	CEnrollDlg(const device_t &dev, CWnd *parent);
	~CEnrollDlg() override;

protected:
	BOOL OnInitDialog() override;
	afx_msg void OnCopy();
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg HBRUSH OnCtlColor(CDC *pDC, CWnd *pWnd, UINT type);
	afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT dis);
	afx_msg void OnDestroy();

	DECLARE_MESSAGE_MAP()

private:
	static CString Hex(const uint8_t *buf, int len);

	CString m_id;
	CString m_key;
	ui::Fonts m_fonts;
	CBrush m_card_brush;
};
