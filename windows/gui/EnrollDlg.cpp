#include "framework.h"
#include "resource.h"
#include "EnrollDlg.h"

BEGIN_MESSAGE_MAP(CEnrollDlg, CDialogEx)
	ON_BN_CLICKED(IDC_ENROLL_COPY, &CEnrollDlg::OnCopy)
END_MESSAGE_MAP()

CEnrollDlg::CEnrollDlg(const device_t &dev, CWnd *parent)
	: CDialogEx(IDD_ENROLL, parent)
	, m_id(Hex(dev.id, DEVICE_ID_LEN))
	, m_key(Hex(dev.key, AUTH_KEY_LEN))
{
}

/* CString 은 해제할 때 내용을 지우지 않는다. 키의 16진 문자열이 해제된 힙에 남지
 * 않게 직접 지운다. */
CEnrollDlg::~CEnrollDlg()
{
	int len = m_key.GetLength();

	SecureZeroMemory(m_key.GetBuffer(len), len * sizeof(TCHAR));
	m_key.ReleaseBuffer(0);
}

CString CEnrollDlg::Hex(const uint8_t *buf, int len)
{
	CString s;
	CString t;

	for (int i = 0; i < len; i++) {
		t.Format(_T("%02x"), buf[i]);
		s += t;
	}

	return s;
}

BOOL CEnrollDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	/* 0 과 O, 1 과 l 이 구분되도록 고정폭으로 보여 준다. */
	m_mono.CreatePointFont(90, _T("Consolas"));
	GetDlgItem(IDC_ENROLL_ID)->SetFont(&m_mono);
	GetDlgItem(IDC_ENROLL_KEY)->SetFont(&m_mono);

	SetDlgItemText(IDC_ENROLL_ID, m_id);
	SetDlgItemText(IDC_ENROLL_KEY, m_key);

	return TRUE;
}

/* "<id> <key>" 한 줄을 클립보드에 넣는다.
 *
 * 클립보드에 넘긴 메모리는 SetClipboardData 가 성공하는 순간 시스템 소유가 된다.
 * 그 뒤로는 우리가 해제하면 안 되고, 실패했을 때만 직접 해제한다. */
void CEnrollDlg::OnCopy()
{
	CString line = m_id + _T(" ") + m_key;
	size_t bytes = ((size_t)line.GetLength() + 1) * sizeof(wchar_t);
	HGLOBAL mem;

	if (!OpenClipboard()) {
		AfxMessageBox(_T("클립보드를 열 수 없습니다."), MB_ICONWARNING);
		return;
	}

	EmptyClipboard();
	mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (mem) {
		memcpy(GlobalLock(mem), (LPCWSTR)line, bytes);
		GlobalUnlock(mem);
		if (!SetClipboardData(CF_UNICODETEXT, mem)) {
			GlobalFree(mem);
		}
	}
	CloseClipboard();

	SecureZeroMemory(line.GetBuffer(), bytes);
	line.ReleaseBuffer(0);

	SetDlgItemText(IDC_ENROLL_COPY, _T("복사됨"));
}
