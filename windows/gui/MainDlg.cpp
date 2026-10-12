#include "framework.h"
#include <uxtheme.h>
#include "resource.h"
#include "MainDlg.h"
#include "EnrollDlg.h"

#ifdef _MSC_VER
#pragma comment(lib, "uxtheme.lib")
#endif

#define TUN_PREFIX_LEN 24

/* 리스트 열 정의. 폭은 다이얼로그 단위로 적고 MapDialogRect 로 픽셀로 바꾼다 —
 * 고정 픽셀로 적으면 고DPI 에서 글자가 잘린다. */
static const struct {
	LPCTSTR title;
	int du_width;
	int format;
} COLUMNS[] = {
	{ _T("#"),        28, LVCFMT_RIGHT },
	{ _T("시간"),     44, LVCFMT_RIGHT },
	{ _T("방향"),     34, LVCFMT_CENTER },
	{ _T("타입"),     52, LVCFMT_LEFT },
	{ _T("세션"),     46, LVCFMT_LEFT },
	{ _T("counter"),  40, LVCFMT_RIGHT },
	{ _T("요약"),    150, LVCFMT_LEFT },
	{ _T("길이"),     30, LVCFMT_RIGHT },
	{ _T("결과"),     64, LVCFMT_LEFT },
};

#define COLUMN_COUNT ((int)(sizeof(COLUMNS) / sizeof(COLUMNS[0])))

/* 색을 따로 입히는 열 */
#define COL_DIR		2
#define COL_SUMMARY	6
#define COL_TYPE	3

CMainDlg::CMainDlg(CWnd *parent)
	: CDialogEx(IDD_MAIN, parent)
	, m_session(nullptr)
	, m_state(TUNNEL_STATE_STOPPED)
	, m_connected_at(0)
	, m_has_reported(false)
	, m_deny_reason(0)
	, m_posture_ticks(0)
	, m_session_idx(0)
	, m_stopping(false)
	, m_tabs(nullptr)
	, m_sep_y(0)
	, m_status_dot(0)
	, m_status_sub_color(0)
{
	ZeroMemory(&m_fonts, sizeof(m_fonts));
	ZeroMemory(&m_local, sizeof(m_local));
	ZeroMemory(&m_reported, sizeof(m_reported));
	ZeroMemory(m_posture_bad, sizeof(m_posture_bad));
}

void CMainDlg::DoDataExchange(CDataExchange *pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PACKETS, m_packets);
	DDX_Control(pDX, IDC_POSTURE, m_posture);
	DDX_Control(pDX, IDC_LOG, m_log);
	DDX_Control(pDX, IDC_DETAIL, m_detail);
}

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
	ON_BN_CLICKED(IDC_CONNECT, &CMainDlg::OnConnect)
	ON_BN_CLICKED(IDC_CLEAR, &CMainDlg::OnClear)
	ON_BN_CLICKED(IDC_SAVE, &CMainDlg::OnSave)
	ON_BN_CLICKED(IDC_ENROLL, &CMainDlg::OnEnroll)
	ON_BN_CLICKED(IDC_HIDE_KA, &CMainDlg::OnFilterChanged)
	ON_BN_CLICKED(IDC_ONLY_DROPS, &CMainDlg::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_FILTER_DIR, &CMainDlg::OnFilterChanged)
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_WM_TIMER()
	ON_WM_CTLCOLOR()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_DRAWITEM()
	ON_WM_SYSCOLORCHANGE()
	ON_MESSAGE(WM_PRINTCLIENT, &CMainDlg::OnPrintClient)
	ON_NOTIFY(LVN_GETEMPTYMARKUP, IDC_PACKETS, &CMainDlg::OnGetEmptyMarkup)
	ON_CONTROL(UI_TABS_CHANGED, IDC_TAB, &CMainDlg::OnTabChanged)
	ON_NOTIFY(LVN_GETDISPINFO, IDC_PACKETS, &CMainDlg::OnGetDispInfo)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PACKETS, &CMainDlg::OnPacketCustomDraw)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_PACKETS, &CMainDlg::OnPacketSelChanged)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_POSTURE, &CMainDlg::OnPostureCustomDraw)
	ON_MESSAGE(WM_VPN_LOG, &CMainDlg::OnVpnLog)
	ON_MESSAGE(WM_VPN_STATE, &CMainDlg::OnVpnState)
	ON_MESSAGE(WM_VPN_DONE, &CMainDlg::OnVpnDone)
	ON_MESSAGE(WM_VPN_PACKETS, &CMainDlg::OnVpnPackets)
END_MESSAGE_MAP()

BOOL CMainDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	/* 큰 아이콘(Alt+Tab)과 작은 아이콘(제목 표시줄)을 각자의 크기로 읽는다. LoadIcon 은
	 * 32픽셀 하나만 주므로 작은 쪽은 줄여 그려져 뭉개진다. */
	SetIcon(AfxGetApp()->LoadIcon(IDI_APP), TRUE);
	SetIcon(static_cast<HICON>(LoadImageW(AfxGetInstanceHandle(), MAKEINTRESOURCEW(IDI_APP),
			IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
			LR_SHARED)), FALSE);
	ui::MatchTitleBar(GetSafeHwnd());

	/* 글꼴은 다이얼로그 글꼴에서 파생한다. 그 글꼴은 이미 시스템 DPI 에 맞춰져 있다. */
	ui::CreateFonts(reinterpret_cast<HFONT>(SendMessage(WM_GETFONT)), &m_fonts);
	m_card_brush.CreateSolidBrush(ui::Colors().card);

	m_tabs = ::GetDlgItem(GetSafeHwnd(), IDC_TAB);
	ui::TabsAdd(m_tabs, L"패킷");
	ui::TabsAdd(m_tabs, L"로그");
	ui::TabsAdd(m_tabs, L"포스처");

	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_CONNECT), ui::BUTTON_PRIMARY);
	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_CLEAR), ui::BUTTON_SECONDARY);
	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_SAVE), ui::BUTTON_SECONDARY);
	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_ENROLL), ui::BUTTON_SECONDARY);

	CComboBox *filter = (CComboBox *)GetDlgItem(IDC_FILTER_DIR);
	filter->AddString(_T("전체"));
	filter->AddString(_T("송신만"));
	filter->AddString(_T("수신만"));
	filter->SetCurSel(PKT_FILTER_ALL);

	/* 빈 칸에 무엇을 적는지 흐린 글씨로 알려 준다. */
	GetDlgItem(IDC_SERVER)->SendMessage(EM_SETCUEBANNER, FALSE, (LPARAM)L"203.0.113.10");
	GetDlgItem(IDC_PORT)->SendMessage(EM_SETCUEBANNER, FALSE, (LPARAM)L"9000");
	GetDlgItem(IDC_TUNIP)->SendMessage(EM_SETCUEBANNER, FALSE, (LPARAM)L"10.0.0.3");

	SetupPacketColumns();

	/* 가상 리스트: 행을 컨트롤에 복사하지 않고 그릴 때마다 LVN_GETDISPINFO 로
	 * 물어본다. 수만 행이 되어도 컨트롤 쪽 메모리와 삽입 비용이 늘지 않는다.
	 * DOUBLEBUFFER 는 빠르게 추가될 때의 깜빡임을 없앤다. */
	m_packets.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
			LVS_EX_HEADERDRAGDROP);
	StyleList(m_packets);

	m_detail.SetFont(CFont::FromHandle(m_fonts.mono));
	m_log.SetFont(CFont::FromHandle(m_fonts.mono));

	SetupPostureList();
	LoadDeviceId();
	RefreshPosture(true);

	LoadSettings();

	/* 창이 만들어진 뒤여야 PostMessage 대상 HWND 를 넘길 수 있다. */
	m_session = vpn_session_create(GetSafeHwnd());
	if (!m_session) {
		AfxMessageBox(_T("세션을 만들 수 없습니다."), MB_ICONERROR);
		EndDialog(IDCANCEL);
		return TRUE;
	}

	SetUiState(TUNNEL_STATE_STOPPED);
	UpdateStats();
	SetTimer(TIMER_TICK_ID, TIMER_TICK_MS, nullptr);

	/* 탭에 따라 보이는 컨트롤이 정해지고, 그 뒤에 자리를 잡는다. */
	OnTabChanged();

	return TRUE;
}

/* 리스트를 탐색기와 같은 모양으로 (선택 / 마우스 올림 표시가 납작해진다) 바꾸고 행을
 * 조금 높인다. 리스트뷰에는 행 높이를 정하는 설정이 없다. 행은 글꼴과 작은 아이콘
 * 중 큰 쪽에 맞춰지므로, 높이만 있는 빈 이미지 목록을 물려 준다. */
void CMainDlg::StyleList(CListCtrl &list)
{
	SetWindowTheme(list.GetSafeHwnd(), L"Explorer", nullptr);

	if (!m_row_pad.GetSafeHandle()) {
		m_row_pad.Create(1, ui::Scale(22), ILC_COLOR32, 0, 1);
	}
	list.SetImageList(&m_row_pad, LVSIL_SMALL);
}

void CMainDlg::SetupPacketColumns()
{
	CRect unit(0, 0, 4, 8);
	MapDialogRect(&unit);

	for (int i = 0; i < COLUMN_COUNT; i++) {
		int px = COLUMNS[i].du_width * unit.Width() / 4;
		m_packets.InsertColumn(i, COLUMNS[i].title, COLUMNS[i].format, px);
	}
}

/* ---- 설정 저장 / 복원 --------------------------------------------------- */
void CMainDlg::LoadSettings()
{
	CWinApp *app = AfxGetApp();

	SetDlgItemText(IDC_ADAPTER, app->GetProfileString(_T("conn"), _T("adapter"), _T("my_vpn")));
	SetDlgItemText(IDC_SERVER, app->GetProfileString(_T("conn"), _T("server"), _T("49.247.139.39")));
	SetDlgItemText(IDC_PORT, app->GetProfileString(_T("conn"), _T("port"), _T("9000")));
	SetDlgItemText(IDC_TUNIP, app->GetProfileString(_T("conn"), _T("tunip"), _T("10.0.0.3")));
}

void CMainDlg::SaveSettings()
{
	CWinApp *app = AfxGetApp();
	CString s;

	GetDlgItemText(IDC_ADAPTER, s);
	app->WriteProfileString(_T("conn"), _T("adapter"), s);
	GetDlgItemText(IDC_SERVER, s);
	app->WriteProfileString(_T("conn"), _T("server"), s);
	GetDlgItemText(IDC_PORT, s);
	app->WriteProfileString(_T("conn"), _T("port"), s);
	GetDlgItemText(IDC_TUNIP, s);
	app->WriteProfileString(_T("conn"), _T("tunip"), s);
}

/* ---- 설정 읽기 ---------------------------------------------------------- */
bool CMainDlg::BuildConfig(tunnel_config_t &cfg)
{
	CString adapter;
	CString server;
	CString port;
	CString tun_ip;
	int port_num;

	GetDlgItemText(IDC_ADAPTER, adapter);
	GetDlgItemText(IDC_SERVER, server);
	GetDlgItemText(IDC_PORT, port);
	GetDlgItemText(IDC_TUNIP, tun_ip);

	adapter.Trim();
	server.Trim();
	port.Trim();
	tun_ip.Trim();

	ZeroMemory(&cfg, sizeof(cfg));
	cfg.prefix_len = TUN_PREFIX_LEN;
	/* on_packet 을 쓰므로 코어의 패킷 텍스트 로그는 필요 없다. */
	cfg.log_packets = 0;

	if (adapter.IsEmpty() || adapter.GetLength() >= TUNNEL_ADAPTER_NAME_LEN) {
		FieldError(IDC_ADAPTER, _T("터널 어댑터의 이름입니다. 비워 둘 수 없습니다."));
		return false;
	}
	/* adapter_name 은 wchar_t 배열이고 cfg 는 구조체째로 복사되므로
	 * CString 의 수명에 의존하지 않는다. */
	wcscpy_s(cfg.adapter_name, TUNNEL_ADAPTER_NAME_LEN, adapter);

	if (InetPtonW(AF_INET, server, &cfg.server_ip) != 1) {
		FieldError(IDC_SERVER, _T("서버의 IPv4 주소를 적어 주세요. 예) 203.0.113.10"));
		return false;
	}

	port_num = _ttoi(port);
	if (port_num <= 0 || port_num > 65535) {
		FieldError(IDC_PORT, _T("1 부터 65535 사이의 숫자여야 합니다."));
		return false;
	}
	cfg.server_port = static_cast<uint16_t>(port_num);

	if (InetPtonW(AF_INET, tun_ip, &cfg.tun_ip) != 1) {
		FieldError(IDC_TUNIP, _T("서버가 이 기기에 배정한 터널 IPv4 주소를 적어 주세요. 예) 10.0.0.3"));
		return false;
	}

	/* 기기 신원 */
	device_t dev;
	int enrolled = device_load(&dev);

	if (enrolled == DEVICE_NOT_ENROLLED) {
		AfxMessageBox(_T("등록되지 않은 기기입니다.\n\n")
				_T("[포스처] 탭의 [기기 등록...] 으로 ID 와 키를 만들고\n")
				_T("그 값을 서버에 등록하세요."), MB_ICONWARNING);
		/* 어디로 가야 하는지 말로만 하지 않고 데려다 준다. */
		ui::TabsSetSel(m_tabs, TAB_POSTURE);
		OnTabChanged();
		return false;
	}
	if (enrolled != DEVICE_OK) {
		AfxMessageBox(_T("기기 신원을 읽지 못했습니다."), MB_ICONWARNING);
		return false;
	}
	memcpy(cfg.device_id, dev.id, sizeof(cfg.device_id));
	memcpy(cfg.device_key, dev.key, sizeof(cfg.device_key));
	SecureZeroMemory(&dev, sizeof(dev));

	return true;
}

/* ---- 버튼 -------------------------------------------------------------- */
/* 버튼은 하나다. 터널이 닫혀 있으면 "연결", 열려 있으면 "연결 해제"다. 두 버튼을
 * 나란히 두면 늘 한쪽은 눌리지 않는 채로 자리만 차지한다. */
void CMainDlg::OnConnect()
{
	tunnel_config_t cfg;

	if (vpn_session_active(m_session)) {
		RequestDisconnect();
		return;
	}

	if (!BuildConfig(cfg)) {
		return;
	}

	SaveSettings();

	/* 지난 연결의 판정과 보고 값은 새 연결과 무관하다. */
	m_deny_reason = 0;
	m_has_reported = false;
	m_session_idx = 0;

	if (vpn_session_start(m_session, &cfg) < 0) {
		AppendLog(_T("[gui] 워커 스레드를 만들 수 없습니다"));
		return;
	}

	/* tunnel_run 도 곧 CONNECTING 을 보내지만, 버튼을 누른 즉시 반응을 보이도록
	 * 여기서 한 번 더 바꿔 준다. */
	SetUiState(TUNNEL_STATE_CONNECTING);
}

void CMainDlg::RequestDisconnect()
{
	if (m_stopping) {
		return;
	}

	AppendLog(_T("[gui] 해제 요청"));
	vpn_session_stop(m_session);

	/* 여기서 워커를 기다리지 않는다. 기다리면 UI 가 멈추고, 그 사이 워커가 보낸
	 * PostMessage 도 처리되지 않는다. 워커가 끝나면 WM_VPN_DONE 이 오고 거기서 join 한다.
	 * 그동안 버튼은 잠그고 무슨 일이 진행 중인지 적어 둔다. */
	m_stopping = true;
	SetUiState(m_state);
}

/* 잘못 적은 칸을 가리키며 말한다. 메시지 상자는 확인을 누르고 나면 어느 칸이었는지
 * 다시 찾아야 한다. */
void CMainDlg::FieldError(int id, LPCTSTR message)
{
	CWnd *edit = GetDlgItem(id);
	EDITBALLOONTIP tip;

	ZeroMemory(&tip, sizeof(tip));
	tip.cbStruct = sizeof(tip);
	tip.pszTitle = L"입력을 확인해 주세요";
	tip.pszText = message;
	tip.ttiIcon = TTI_WARNING;

	edit->SetFocus();
	edit->SendMessage(EM_SETSEL, 0, -1);
	edit->SendMessage(EM_SHOWBALLOONTIP, 0, reinterpret_cast<LPARAM>(&tip));
}

void CMainDlg::OnClear()
{
	m_store.Clear();
	m_log.ResetContent();
	m_detail.SetWindowText(_T(""));
	RefreshPacketCount();
	UpdateStats();
}

void CMainDlg::OnSave()
{
	CFileDialog dlg(FALSE, _T("csv"), _T("my_vpn_packets.csv"),
			OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
			_T("CSV 파일 (*.csv)|*.csv|모든 파일 (*.*)|*.*||"), this);

	if (dlg.DoModal() != IDOK) {
		return;
	}

	CStdioFile f;
	if (!f.Open(dlg.GetPathName(), CFile::modeCreate | CFile::modeWrite | CFile::typeBinary)) {
		AfxMessageBox(_T("파일을 열 수 없습니다."), MB_ICONERROR);
		return;
	}

	try {
		/* 엑셀이 한글을 알아보게 UTF-8 BOM 을 먼저 쓴다. */
		const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
		f.Write(bom, 3);

		CString line;
		for (int c = 0; c < COLUMN_COUNT; c++) {
			line += COLUMNS[c].title;
			line += (c + 1 < COLUMN_COUNT) ? _T(",") : _T("\r\n");
		}

		CStringA utf8;
		int n = m_store.ViewCount();
		for (int i = -1; i < n; i++) {
			if (i >= 0) {
				const tunnel_packet_t *p = m_store.ViewAt(i);
				line.Empty();
				for (int c = 0; c < COLUMN_COUNT; c++) {
					CString v = ColumnText(*p, c);
					v.Replace(_T("\""), _T("\"\""));
					line += _T("\"") + v + _T("\"");
					line += (c + 1 < COLUMN_COUNT) ? _T(",") : _T("\r\n");
				}
			}

			int need = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
			if (need > 1) {
				char *buf = utf8.GetBufferSetLength(need);
				WideCharToMultiByte(CP_UTF8, 0, line, -1, buf, need, nullptr, nullptr);
				f.Write(buf, (UINT)(need - 1));
			}
		}
		f.Close();
	}
	catch (CFileException *e) {
		e->Delete();
		AfxMessageBox(_T("저장 중 오류가 발생했습니다."), MB_ICONERROR);
		return;
	}

	AppendLog(_T("[gui] 저장 완료: ") + dlg.GetPathName());
}

void CMainDlg::OnFilterChanged()
{
	ApplyFilter();
}

/* 이 기기의 ID 와 키를 새로 만든다. */
void CMainDlg::OnEnroll()
{
	device_t dev;

	/* 워커가 설정의 사본에 예전 키를 들고 돌고 있다. 지금 키를 바꾸면 화면의
	 * 기기 ID 와 실제로 쓰이는 키가 어긋난다. */
	if (vpn_session_active(m_session)) {
		AfxMessageBox(_T("연결을 해제한 뒤에 등록하세요."), MB_ICONINFORMATION);
		return;
	}

	if (!m_device_id.IsEmpty()) {
		if (AfxMessageBox(_T("이미 등록된 기기입니다.\n\n")
				_T("새 키를 만들면 서버에 등록해 둔 지금의 키로는 접속할 수 없게 됩니다.\n")
				_T("새로 만들까요?"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
			return;
		}
	}

	if (device_enroll(&dev) != DEVICE_OK) {
		AfxMessageBox(_T("기기 등록에 실패했습니다."), MB_ICONERROR);
		return;
	}

	{
		CEnrollDlg dlg(dev, this);

		SecureZeroMemory(&dev, sizeof(dev));
		dlg.DoModal();
	}

	LoadDeviceId();
	RefreshPosture(false);
	AppendLog(_T("[gui] 기기 등록: ") + m_device_id);
}

void CMainDlg::ApplyFilter()
{
	CComboBox *filter = (CComboBox *)GetDlgItem(IDC_FILTER_DIR);
	int dir = filter ? filter->GetCurSel() : PKT_FILTER_ALL;

	if (dir < 0) {
		dir = PKT_FILTER_ALL;
	}

	m_store.SetFilter(dir,
			IsDlgButtonChecked(IDC_HIDE_KA) == BST_CHECKED,
			IsDlgButtonChecked(IDC_ONLY_DROPS) == BST_CHECKED);

	m_detail.SetWindowText(_T(""));
	RefreshPacketCount();
}

void CMainDlg::RefreshPacketCount()
{
	if (!m_packets.GetSafeHwnd()) {
		return;
	}

	int n = m_store.ViewCount();

	/* 가상 리스트는 "몇 행인지"만 알려주면 된다. */
	m_packets.SetItemCountEx(n, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
	m_packets.Invalidate(FALSE);

	/* 선택이 없으면 최신 행을 따라간다. 사용자가 뭔가 골라 보고 있으면 건드리지 않는다. */
	if (n > 0 && m_packets.GetSelectedCount() == 0) {
		m_packets.EnsureVisible(n - 1, FALSE);
	}
}

/* ---- 워커 -> UI -------------------------------------------------------- */
LRESULT CMainDlg::OnVpnLog(WPARAM, LPARAM lParam)
{
	char *line = reinterpret_cast<char *>(lParam);

	if (line) {
		/* 워커가 넘긴 소유권이 여기서 끝난다. */
		AppendLog(CString(line));
		vpn_session_free_log(line);
	}

	return 0;
}

LRESULT CMainDlg::OnVpnState(WPARAM wParam, LPARAM)
{
	tunnel_state_t st = static_cast<tunnel_state_t>(wParam);

	if (st == TUNNEL_STATE_RECONNECTING) {
		m_store.CountRehandshake();
	}
	SetUiState(st);

	return 0;
}

LRESULT CMainDlg::OnVpnDone(WPARAM wParam, LPARAM)
{
	const int ret = static_cast<int>(wParam);

	/* 워커는 이미 반환했으므로 join 은 바로 돌아온다. 핸들을 정리해야 다시 연결할 수 있다. */
	vpn_session_join(m_session, VPN_STOP_TIMEOUT_MS);

	/* 종료 직전에 올라온 패킷이 링에 남아 있을 수 있다. */
	OnVpnPackets(0, 0);

	AppendLog(ret == 0 ? _T("[gui] 정상 종료") : _T("[gui] 오류로 종료"));
	SetUiState(TUNNEL_STATE_STOPPED);

	return 0;
}

LRESULT CMainDlg::OnVpnPackets(WPARAM, LPARAM)
{
	tunnel_packet_t batch[PACKET_DRAIN_BATCH];
	int n;

	/* 0 이 나올 때까지 비운다. 한 번에 전부 가져오지 않고 배치로 끊어
	 * UI 가 한 번에 오래 멈추지 않게 한다. */
	do {
		n = vpn_session_drain_packets(m_session, batch, PACKET_DRAIN_BATCH);
		for (int i = 0; i < n; i++) {
			m_store.Add(batch[i]);
			TrackAuth(batch[i]);
		}
	} while (n == PACKET_DRAIN_BATCH);

	m_store.SetLost(vpn_session_lost_packets(m_session));
	RefreshPacketCount();

	/* 상태 변화(WM_VPN_STATE)와 패킷(WM_VPN_PACKETS)은 따로 오는 메시지라, "거부됨"
	 * 상태가 DENY 패킷보다 먼저 처리될 수 있다. 그때는 SetUiState 가 사유를 아직
	 * 모르므로 사유를 알게 된 여기서도 한 번 더 쓴다. */
	UpdateStatus();
	RefreshPosture(false);

	return 0;
}

/* ---- 가상 리스트 ------------------------------------------------------- */
void CMainDlg::OnGetDispInfo(NMHDR *pNMHDR, LRESULT *pResult)
{
	NMLVDISPINFO *di = reinterpret_cast<NMLVDISPINFO *>(pNMHDR);

	*pResult = 0;

	if (!(di->item.mask & LVIF_TEXT)) {
		return;
	}

	const tunnel_packet_t *p = m_store.ViewAt(di->item.iItem);
	if (!p) {
		di->item.pszText[0] = _T('\0');
		return;
	}

	CString s = ColumnText(*p, di->item.iSubItem);
	_tcsncpy_s(di->item.pszText, di->item.cchTextMax, s, _TRUNCATE);
}

/* 행 전체를 방향별로 칠하지 않는다. 표가 줄무늬로 가득 차면 정작 봐야 할 폐기 행이
 * 묻힌다. 방향은 그 열의 글자색으로만 알리고, 바탕을 칠하는 것은 폐기된 행뿐이다. */
void CMainDlg::OnPacketCustomDraw(NMHDR *pNMHDR, LRESULT *pResult)
{
	LPNMLVCUSTOMDRAW cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(pNMHDR);
	const ui::Palette &c = ui::Colors();

	*pResult = CDRF_DODEFAULT;

	/* 고대비에서는 시스템이 고른 색을 그대로 둔다. 폐기 여부는 결과 열에 글자로 있다. */
	if (c.highContrast) {
		return;
	}

	switch (cd->nmcd.dwDrawStage) {
	case CDDS_PREPAINT:
		*pResult = CDRF_NOTIFYITEMDRAW;
		return;

	case CDDS_ITEMPREPAINT:
		*pResult = CDRF_NOTIFYSUBITEMDRAW;
		return;

	case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
		const tunnel_packet_t *p = m_store.ViewAt((int)cd->nmcd.dwItemSpec);

		if (!p) {
			return;
		}

		if (p->verdict != TUNNEL_PASS) {
			cd->clrTextBk = c.dangerBg;
			cd->clrText = c.danger;
		}
		else {
			cd->clrTextBk = c.card;
			if (cd->iSubItem == COL_DIR) {
				cd->clrText = (p->dir == TUNNEL_DIR_TX) ? c.tx : c.rx;
			}
			else if (cd->iSubItem == COL_SUMMARY || cd->iSubItem == COL_TYPE) {
				cd->clrText = c.text;
			}
			else {
				/* 번호, 시간, 세션, counter, 길이, 결과는 한 단계 흐리게. 눈이
				 * 타입과 요약으로 먼저 간다. */
				cd->clrText = c.muted;
			}
		}
		*pResult = CDRF_NEWFONT;
		return;
	}
	default:
		return;
	}
}

/* 리스트가 비어 있을 때 한가운데에 보일 글. 빈 표만 있으면 고장인지 아직 아무 일도
 * 없었던 것인지 알 수 없다. */
void CMainDlg::OnGetEmptyMarkup(NMHDR *pNMHDR, LRESULT *pResult)
{
	NMLVEMPTYMARKUP *em = reinterpret_cast<NMLVEMPTYMARKUP *>(pNMHDR);

	em->dwFlags = EMF_CENTERED;
	wcsncpy_s(em->szMarkup, ARRAYSIZE(em->szMarkup),
			L"표시할 패킷이 없습니다. 연결하면 터널을 오가는 패킷이 여기에 나타납니다.", _TRUNCATE);
	*pResult = TRUE;
}

void CMainDlg::OnPacketSelChanged(NMHDR *pNMHDR, LRESULT *pResult)
{
	NMLISTVIEW *lv = reinterpret_cast<NMLISTVIEW *>(pNMHDR);

	*pResult = 0;

	if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) {
		ShowDetail(lv->iItem);
	}
}

/* 탭마다 필요한 것만 보인다. 필터와 상세창은 패킷에만 뜻이 있으므로 다른 탭에서는
 * 치우고 그 자리를 내용에 준다. */
void CMainDlg::OnTabChanged()
{
	static const int packetsOnly[] = { IDC_FILTER_DIR, IDC_HIDE_KA, IDC_ONLY_DROPS, IDC_SAVE, IDC_DETAIL };
	const int sel = ui::TabsGetSel(m_tabs);
	CRect rc;

	m_packets.ShowWindow(sel == TAB_PACKETS ? SW_SHOW : SW_HIDE);
	m_log.ShowWindow(sel == TAB_LOG ? SW_SHOW : SW_HIDE);
	m_posture.ShowWindow(sel == TAB_POSTURE ? SW_SHOW : SW_HIDE);

	for (int id : packetsOnly) {
		GetDlgItem(id)->ShowWindow(sel == TAB_PACKETS ? SW_SHOW : SW_HIDE);
	}
	GetDlgItem(IDC_CLEAR)->ShowWindow(sel != TAB_POSTURE ? SW_SHOW : SW_HIDE);
	GetDlgItem(IDC_ENROLL)->ShowWindow(sel == TAB_POSTURE ? SW_SHOW : SW_HIDE);

	if (sel == TAB_POSTURE) {
		RefreshPosture(true);
	}

	GetClientRect(&rc);
	LayoutChildren(rc.Width(), rc.Height());
}

/* ---- 포스처 탭 --------------------------------------------------------- */
/* 패킷 리스트와 달리 행이 다섯 개로 고정이라 가상 리스트를 쓰지 않는다. 행을 미리
 * 넣어 두고 값이 바뀐 칸만 고친다. */
void CMainDlg::SetupPostureList()
{
	static const struct {
		LPCTSTR title;
		int du_width;
	} cols[] = {
		{ _T("항목"),               70 },
		{ _T("이 PC (지금)"),       190 },
		{ _T("서버에 보고한 값"),   190 },
	};
	static const LPCTSTR rows[POSTURE_ROW_COUNT] = {
		_T("기기 ID"), _T("OS 빌드"), _T("방화벽"), _T("백신"), _T("서버 판정"),
	};
	CRect unit(0, 0, 4, 8);

	MapDialogRect(&unit);
	m_posture.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	StyleList(m_posture);

	for (int i = 0; i < (int)(sizeof(cols) / sizeof(cols[0])); i++) {
		m_posture.InsertColumn(i, cols[i].title, LVCFMT_LEFT, cols[i].du_width * unit.Width() / 4);
	}
	for (int i = 0; i < POSTURE_ROW_COUNT; i++) {
		m_posture.InsertItem(i, rows[i]);
	}
}

/* 기기 ID 만 보여 준다. device_load 는 키까지 풀어 주므로 쓰지 않을 키는 바로 지운다. */
void CMainDlg::LoadDeviceId()
{
	device_t dev;

	m_device_id.Empty();

	if (device_load(&dev) == DEVICE_OK) {
		CString t;
		for (int i = 0; i < DEVICE_ID_LEN; i++) {
			t.Format(_T("%02x"), dev.id[i]);
			m_device_id += t;
		}
	}
	SecureZeroMemory(&dev, sizeof(dev));
}

/* 패킷 이벤트에서 "무엇을 보고했고 서버가 뭐라고 했는지"를 주워 둔다.
 * 코어에 콜백을 더 만들지 않았다. 필요한 사실이 이미 패킷으로 올라온다. */
void CMainDlg::TrackAuth(const tunnel_packet_t &p)
{
	if (p.dir == TUNNEL_DIR_TX && p.has_posture) {
		m_reported = p.posture;
		m_has_reported = true;
	}

	if (p.dir != TUNNEL_DIR_RX || p.verdict != TUNNEL_PASS) {
		return;
	}
	if (p.msg_type == MSG_TYPE_DENY) {
		m_deny_reason = p.deny_reason;
	}
	else if (p.msg_type == MSG_TYPE_RES_HANDSHAKE) {
		m_deny_reason = 0;
		m_session_idx = p.session_idx;
	}
}

CString CMainDlg::LocalStateName(int state)
{
	switch (state) {
	case POSTURE_ON:	return _T("켜짐");
	case POSTURE_OFF:	return _T("꺼짐");
	default:		return _T("확인 불가");
	}
}

CString CMainDlg::ServerVerdictText() const
{
	if (m_deny_reason != 0) {
		CString s = _T("거부 — ") + DenyReasonName(m_deny_reason);

		if (m_state == TUNNEL_STATE_STOPPED) {
			s += _T(" (연결 종료)");
		}
		return s;
	}

	switch (m_state) {
	case TUNNEL_STATE_CONNECTED:	return _T("허용");
	case TUNNEL_STATE_CONNECTING:	return _T("확인 중");
	case TUNNEL_STATE_RECONNECTING:	return _T("재연결 중");
	case TUNNEL_STATE_DENIED:	return _T("거부");
	default:			return _T("연결 안 됨");
	}
}

/* 값이 그대로면 건드리지 않는다. 2초마다 같은 글자를 다시 쓰면 선택 표시가 깜빡인다. */
void CMainDlg::SetPostureCell(int row, int col, const CString &text)
{
	if (m_posture.GetItemText(row, col) != text) {
		m_posture.SetItemText(row, col, text);
	}
}

/* collect 가 true 면 이 PC 의 상태를 다시 읽는다. 수십 ms 걸리는 호출이라 패킷이
 * 올 때마다 하지 않고 타이머와 탭 전환에서만 한다. */
void CMainDlg::RefreshPosture(bool collect)
{
	bool bad[POSTURE_ROW_COUNT] = { false };
	CString s;

	if (!m_posture.GetSafeHwnd()) {
		return;
	}

	if (collect) {
		posture_collect(&m_local);
	}

	SetPostureCell(POSTURE_ROW_DEVICE, 1,
			m_device_id.IsEmpty() ? CString(_T("등록되지 않음")) : m_device_id);
	bad[POSTURE_ROW_DEVICE] = m_device_id.IsEmpty();

	if (m_local.os_build != 0) {
		s.Format(_T("%u.%u.%u"), m_local.os_major, m_local.os_minor, m_local.os_build);
	}
	else {
		s = _T("확인 불가");
	}
	SetPostureCell(POSTURE_ROW_OS, 1, s);
	SetPostureCell(POSTURE_ROW_FIREWALL, 1, LocalStateName(m_local.firewall));
	SetPostureCell(POSTURE_ROW_ANTIVIRUS, 1, LocalStateName(m_local.antivirus));
	bad[POSTURE_ROW_FIREWALL] = (m_local.firewall != POSTURE_ON);
	bad[POSTURE_ROW_ANTIVIRUS] = (m_local.antivirus != POSTURE_ON);

	if (m_has_reported) {
		s.Format(_T("%u"), m_reported.os_build);
		SetPostureCell(POSTURE_ROW_OS, 2, s);
		SetPostureCell(POSTURE_ROW_FIREWALL, 2, PostureStateName(m_reported.firewall));
		SetPostureCell(POSTURE_ROW_ANTIVIRUS, 2, PostureStateName(m_reported.antivirus));
	}
	else {
		SetPostureCell(POSTURE_ROW_OS, 2, _T("-"));
		SetPostureCell(POSTURE_ROW_FIREWALL, 2, _T("-"));
		SetPostureCell(POSTURE_ROW_ANTIVIRUS, 2, _T("-"));
	}

	SetPostureCell(POSTURE_ROW_VERDICT, 1, ServerVerdictText());
	bad[POSTURE_ROW_VERDICT] = (m_deny_reason != 0 || m_state == TUNNEL_STATE_DENIED);

	if (memcmp(bad, m_posture_bad, sizeof(bad)) != 0) {
		memcpy(m_posture_bad, bad, sizeof(bad));
		m_posture.Invalidate(FALSE);
	}
}

void CMainDlg::OnPostureCustomDraw(NMHDR *pNMHDR, LRESULT *pResult)
{
	LPNMLVCUSTOMDRAW cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(pNMHDR);

	*pResult = CDRF_DODEFAULT;

	if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
		*pResult = CDRF_NOTIFYITEMDRAW;
	}
	else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
		int row = (int)cd->nmcd.dwItemSpec;

		if (row >= 0 && row < POSTURE_ROW_COUNT && m_posture_bad[row] &&
				!ui::Colors().highContrast) {
			cd->clrTextBk = ui::Colors().dangerBg;
			cd->clrText = ui::Colors().danger;
		}
	}
}

/* ---- 상세창 ------------------------------------------------------------ */
void CMainDlg::ShowDetail(int viewIndex)
{
	const tunnel_packet_t *p = m_store.ViewAt(viewIndex);

	if (!p) {
		m_detail.SetWindowText(_T(""));
		return;
	}

	CString s;
	CString t;

	/* 우리가 설계한 16바이트를 필드별로 되짚는다. */
	if (p->msg_type != 0) {
		t.Format(_T("msg_header   version=%u  type=%u (%s)\r\n"),
				(unsigned)PROTOCOL_VERSION, (unsigned)p->msg_type,
				(LPCTSTR)TypeName(*p));
		s += t;
		t.Format(_T("             session_idx=0x%08x\r\n"), p->session_idx);
		s += t;
	}
	else {
		s += _T("msg_header   (읽지 못함)\r\n");
	}

	if (p->has_counter) {
		t.Format(_T("data_header  counter=%llu\r\n"), p->counter);
		s += t;
	}

	if (p->has_posture) {
		t.Format(_T("posture      os_build=%u  firewall=%u (%s)  antivirus=%u (%s)\r\n"),
				p->posture.os_build,
				(unsigned)p->posture.firewall, (LPCTSTR)PostureStateName(p->posture.firewall),
				(unsigned)p->posture.antivirus, (LPCTSTR)PostureStateName(p->posture.antivirus));
		s += t;
		s += _T("             (뒤의 32바이트는 HMAC-SHA256)\r\n");
	}

	if (p->msg_type == MSG_TYPE_DENY && p->deny_reason != 0) {
		t.Format(_T("deny         reason=%u (%s)\r\n"),
				(unsigned)p->deny_reason, (LPCTSTR)DenyReasonName(p->deny_reason));
		s += t;
	}

	if (p->has_inner) {
		TCHAR src[64] = { 0 };
		TCHAR dst[64] = { 0 };
		InetNtopW(AF_INET, &p->inner_src, src, 64);
		InetNtopW(AF_INET, &p->inner_dst, dst, 64);

		t.Format(_T("inner IPv4   %s -> %s  proto=%u (%s)  len=%u\r\n"),
				src, dst, (unsigned)p->proto, (LPCTSTR)ProtoName(*p), p->inner_len);
		s += t;

		if (p->has_l4) {
			if (p->proto == 1) {
				LPCTSTR icmp = _T("");
				if (p->icmp_type == 8) {
					icmp = _T(" (echo request)");
				}
				else if (p->icmp_type == 0) {
					icmp = _T(" (echo reply)");
				}
				t.Format(_T("ICMP         type=%u%s  code=%u\r\n"),
						(unsigned)p->icmp_type, icmp, (unsigned)p->icmp_code);
			}
			else {
				t.Format(_T("%-12s sport=%u  dport=%u\r\n"),
						(LPCTSTR)ProtoName(*p), (unsigned)p->sport, (unsigned)p->dport);
			}
			s += t;
		}
	}

	t.Format(_T("결과         %s  (와이어 %u 바이트)\r\n\r\n"),
			(LPCTSTR)VerdictName(p->verdict), p->wire_len);
	s += t;

	/* hex 덤프. 앞 TUNNEL_SNAP_LEN 바이트만 떠 두었다. */
	for (int off = 0; off < p->snap_len; off += 16) {
		CString hex;
		CString asc;

		for (int i = 0; i < 16; i++) {
			if (off + i < p->snap_len) {
				BYTE b = p->snap[off + i];
				t.Format(_T("%02x "), b);
				hex += t;
				asc += (b >= 0x20 && b < 0x7f) ? (TCHAR)b : _T('.');
			}
			else {
				hex += _T("   ");
			}
			if (i == 7) {
				hex += _T(" ");
			}
		}
		t.Format(_T("%04x  %s %s\r\n"), off, (LPCTSTR)hex, (LPCTSTR)asc);
		s += t;
	}

	m_detail.SetWindowText(s);
}

/* ---- 표시 문자열 ------------------------------------------------------- */
CString CMainDlg::TimeText(ULONGLONG t_ms)
{
	CString s;

	s.Format(_T("%llu:%02llu.%03llu"), t_ms / 60000, (t_ms / 1000) % 60, t_ms % 1000);

	return s;
}

CString CMainDlg::DirName(const tunnel_packet_t &p)
{
	return (p.dir == TUNNEL_DIR_TX) ? _T("\x2191 송신") : _T("\x2193 수신");
}

CString CMainDlg::TypeName(const tunnel_packet_t &p)
{
	switch (p.msg_type) {
	case MSG_TYPE_REQ_HANDSHAKE:	return _T("REQ_HS");
	case MSG_TYPE_RES_HANDSHAKE:	return _T("RES_HS");
	case MSG_TYPE_DATA:		return _T("DATA");
	case MSG_TYPE_KEEPALIVE:	return _T("KEEPALIVE");
	case MSG_TYPE_CHALLENGE:	return _T("CHALLENGE");
	case MSG_TYPE_AUTH:		return _T("AUTH");
	case MSG_TYPE_DENY:		return _T("DENY");
	default:			return _T("-");
	}
}

CString CMainDlg::PostureStateName(int state)
{
	switch (state) {
	case POSTURE_STATE_ON:		return _T("켜짐");
	case POSTURE_STATE_OFF:		return _T("꺼짐");
	default:			return _T("확인 불가");
	}
}

CString CMainDlg::DenyReasonName(int reason)
{
	switch (reason) {
	case DENY_UNKNOWN_DEVICE:	return _T("등록되지 않은 기기");
	case DENY_BAD_AUTH:		return _T("인증 실패");
	case DENY_TUN_IP:		return _T("이 기기에 배정되지 않은 터널 IP");
	case DENY_POSTURE:		return _T("포스처가 정책에 못 미침");
	default:			return _T("알 수 없는 사유");
	}
}

CString CMainDlg::PostureText(const posture_report_t &r)
{
	CString s;

	s.Format(_T("빌드 %u  방화벽 %s  백신 %s"), r.os_build,
			(LPCTSTR)PostureStateName(r.firewall), (LPCTSTR)PostureStateName(r.antivirus));

	return s;
}

CString CMainDlg::VerdictName(int verdict)
{
	switch (verdict) {
	case TUNNEL_PASS:		return _T("전달");
	case TUNNEL_DROP_NON_IPV4:	return _T("IPv4 아님");
	case TUNNEL_DROP_NON_UNICAST:	return _T("비유니캐스트");
	case TUNNEL_DROP_TOO_LONG:	return _T("길이 초과");
	case TUNNEL_DROP_NOT_SERVER:	return _T("서버 아닌 출발지");
	case TUNNEL_DROP_BAD_HEADER:	return _T("헤더 불량");
	case TUNNEL_DROP_WRONG_SESSION:	return _T("세션 불일치");
	case TUNNEL_DROP_UNKNOWN_TYPE:	return _T("알 수 없는 타입");
	case TUNNEL_DROP_IO_ERROR:	return _T("전달 실패");
	case TUNNEL_DROP_DENIED:	return _T("접근 거부");
	default:			return _T("?");
	}
}

CString CMainDlg::ProtoName(const tunnel_packet_t &p)
{
	switch (p.proto) {
	case 1:		return _T("ICMP");
	case 2:		return _T("IGMP");
	case 6:		return _T("TCP");
	case 17:	return _T("UDP");
	default:	break;
	}

	CString s;
	s.Format(_T("%u"), (unsigned)p.proto);

	return s;
}

CString CMainDlg::Summary(const tunnel_packet_t &p)
{
	/* 인증 메시지는 inner 패킷이 없다. 대신 실어 온 내용을 요약한다. */
	if (p.msg_type == MSG_TYPE_DENY && p.deny_reason != 0) {
		return _T("거부: ") + DenyReasonName(p.deny_reason);
	}
	if (p.has_posture) {
		return PostureText(p.posture);
	}

	if (!p.has_inner) {
		return _T("");
	}

	TCHAR src[64] = { 0 };
	TCHAR dst[64] = { 0 };
	InetNtopW(AF_INET, &p.inner_src, src, 64);
	InetNtopW(AF_INET, &p.inner_dst, dst, 64);

	CString s;
	s.Format(_T("%s \x2192 %s  %s"), src, dst, (LPCTSTR)ProtoName(p));

	if (p.has_l4) {
		CString t;
		if (p.proto == 1) {
			if (p.icmp_type == 8) {
				t = _T(" echo request");
			}
			else if (p.icmp_type == 0) {
				t = _T(" echo reply");
			}
			else {
				t.Format(_T(" type=%u"), (unsigned)p.icmp_type);
			}
		}
		else {
			t.Format(_T(" %u\x2192%u"), (unsigned)p.sport, (unsigned)p.dport);
		}
		s += t;
	}

	return s;
}

CString CMainDlg::ColumnText(const tunnel_packet_t &p, int col) const
{
	CString s;

	switch (col) {
	case 0:
		s.Format(_T("%llu"), p.seq);
		return s;
	case 1:
		return TimeText(p.t_ms);
	case 2:
		return DirName(p);
	case 3:
		return TypeName(p);
	case 4:
		if (p.session_idx == 0) {
			return _T("-");
		}
		s.Format(_T("%08x"), p.session_idx);
		return s;
	case 5:
		if (!p.has_counter) {
			return _T("-");
		}
		s.Format(_T("%llu"), p.counter);
		return s;
	case 6:
		return Summary(p);
	case 7:
		s.Format(_T("%u"), p.wire_len);
		return s;
	case 8:
		return VerdictName(p.verdict);
	default:
		return _T("");
	}
}

/* ---- 상태 / 통계 ------------------------------------------------------- */
void CMainDlg::AppendLog(const CString &text)
{
	if (!m_log.GetSafeHwnd()) {
		return;
	}

	if (m_log.AddString(text) < 0) {
		return;
	}

	while (m_log.GetCount() > LOG_MAX_LINES) {
		m_log.DeleteString(0);
	}

	m_log.SetTopIndex(m_log.GetCount() - 1);
}

void CMainDlg::SetUiState(tunnel_state_t state)
{
	BOOL stopped;
	CWnd *connect = GetDlgItem(IDC_CONNECT);

	if (state == TUNNEL_STATE_CONNECTED && m_connected_at == 0) {
		m_connected_at = GetTickCount64();
	}
	else if (state == TUNNEL_STATE_STOPPED) {
		m_connected_at = 0;
		m_session_idx = 0;
		m_stopping = false;
	}

	m_state = state;
	UpdateStatus();
	RefreshPosture(false);

	stopped = (state == TUNNEL_STATE_STOPPED) ? TRUE : FALSE;

	/* 강조색 버튼은 "지금 할 일"에만 쓴다. 연결된 뒤의 해제는 흔한 동작이 아니므로
	 * 보조 버튼으로 물러난다. */
	connect->SetWindowText(stopped ? _T("연결") : m_stopping ? _T("해제 중…") : _T("연결 해제"));
	connect->EnableWindow(!m_stopping);
	ui::SetButtonKind(connect->GetSafeHwnd(), stopped ? ui::BUTTON_PRIMARY : ui::BUTTON_SECONDARY);
	connect->Invalidate(FALSE);

	/* 리스트나 입력 칸에서 치는 Enter 는 다이얼로그의 기본 버튼으로 간다. 닫혀 있을 때는
	 * 그것이 [연결]이라 편하지만, 열려 있을 때는 같은 버튼이 [연결 해제]다 — 패킷을
	 * 보다가 Enter 를 눌렀다고 터널이 끊겨서는 안 된다. 그동안은 기본 버튼을 버튼이
	 * 아닌 컨트롤로 돌려 둔다 (그쪽으로 가는 BN_CLICKED 는 받는 곳이 없다). */
	SendMessage(DM_SETDEFID, stopped ? IDC_CONNECT : IDC_TAB);

	/* 연결 중에 설정을 바꿔도 반영되지 않으므로 잠근다. */
	GetDlgItem(IDC_ADAPTER)->EnableWindow(stopped);
	GetDlgItem(IDC_SERVER)->EnableWindow(stopped);
	GetDlgItem(IDC_PORT)->EnableWindow(stopped);
	GetDlgItem(IDC_TUNIP)->EnableWindow(stopped);
}

/* 상태 줄의 글과 색을 지금 값으로 다시 정한다. 바뀐 것이 있을 때만 그 영역을 다시
 * 그린다 — 500ms 마다 불리므로 매번 그리면 쓸데없이 깜빡일 틈을 준다. */
void CMainDlg::UpdateStatus()
{
	const ui::Palette &c = ui::Colors();
	CString title;
	CString sub;
	COLORREF dot;
	COLORREF subColor = c.muted;

	switch (m_state) {
	case TUNNEL_STATE_CONNECTING:
		title = _T("연결 중");
		sub = _T("서버에 이 기기를 인증하고 있습니다");
		dot = c.warn;
		break;
	case TUNNEL_STATE_CONNECTED:
		title = _T("연결됨");
		dot = c.ok;
		if (m_connected_at) {
			ULONGLONG sec = (GetTickCount64() - m_connected_at) / 1000;

			sub.Format(_T("%02llu:%02llu:%02llu"), sec / 3600, (sec / 60) % 60, sec % 60);
			if (m_session_idx != 0) {
				CString t;

				t.Format(_T("   ·   세션 %08x"), m_session_idx);
				sub += t;
			}
		}
		break;
	case TUNNEL_STATE_RECONNECTING:
		title = _T("재연결 중");
		sub = _T("서버의 응답이 없어 세션을 다시 맺고 있습니다");
		dot = c.warn;
		break;
	case TUNNEL_STATE_DENIED:
		title = _T("접근 거부됨");
		sub = _T("서버가 이 기기의 접근을 막았습니다");
		dot = c.danger;
		break;
	case TUNNEL_STATE_STOPPED:
	default:
		title = _T("연결되지 않음");
		sub = _T("서버 주소와 터널 IP 를 확인하고 연결을 누르세요");
		dot = c.idle;
		break;
	}

	/* 거부됐으면 그 줄에 사유를 둔다. 탭을 옮기지 않아도 왜 막혔는지 보인다. 연결이
	 * 끊긴 뒤에도 남겨 둔다 — 첫 핸드셰이크에서 거부되면 곧바로 닫히므로, 지우면
	 * 사유를 볼 틈이 없다. 경과 시간으로도 덮지 않는다. */
	if (m_deny_reason != 0) {
		sub = _T("거부 사유: ") + DenyReasonName(m_deny_reason);
		subColor = c.danger;
		if (m_state == TUNNEL_STATE_STOPPED) {
			dot = c.danger;
		}
	}

	if (m_stopping) {
		title = _T("해제 중");
		sub = _T("터널을 닫고 어댑터를 정리하고 있습니다");
		subColor = c.muted;
		dot = c.idle;
	}

	if (title == m_status_title && sub == m_status_sub &&
			dot == m_status_dot && subColor == m_status_sub_color) {
		return;
	}

	m_status_title = title;
	m_status_sub = sub;
	m_status_dot = dot;
	m_status_sub_color = subColor;

	if (!m_rc_status.IsRectEmpty()) {
		InvalidateRect(&m_rc_status, FALSE);
	}
}

static CString BytesText(ULONGLONG bytes)
{
	CString s;

	if (bytes < 1024) {
		s.Format(_T("%llu B"), bytes);
	}
	else if (bytes < 1024 * 1024) {
		s.Format(_T("%.1f KB"), (double)bytes / 1024.0);
	}
	else {
		s.Format(_T("%.1f MB"), (double)bytes / (1024.0 * 1024.0));
	}

	return s;
}

/* 숫자는 타일에, 폐기 사유의 내역은 맨 아래 한 줄에 둔다. 사유는 0 이 아닌 것만 적는다.
 * 일곱 가지를 늘 다 적어 두면 대부분 0 인 숫자 사이에서 하나 있는 2 를 찾아야 한다. */
void CMainDlg::UpdateStats()
{
	static const struct {
		int verdict;
		LPCTSTR name;
	} reasons[] = {
		{ TUNNEL_DROP_DENIED,		_T("접근 거부") },
		{ TUNNEL_DROP_NON_UNICAST,	_T("비유니캐스트") },
		{ TUNNEL_DROP_WRONG_SESSION,	_T("세션 불일치") },
		{ TUNNEL_DROP_NOT_SERVER,	_T("서버 아닌 출발지") },
		{ TUNNEL_DROP_BAD_HEADER,	_T("헤더 불량") },
		{ TUNNEL_DROP_NON_IPV4,		_T("IPv4 아님") },
		{ TUNNEL_DROP_TOO_LONG,		_T("길이 초과") },
		{ TUNNEL_DROP_UNKNOWN_TYPE,	_T("알 수 없는 타입") },
		{ TUNNEL_DROP_IO_ERROR,		_T("전달 실패") },
	};
	const PacketStats &st = m_store.Stats();
	CString value[TILE_COUNT];
	CString note[TILE_COUNT];
	CString footer;
	CString t;

	value[TILE_TX].Format(_T("%llu"), st.tx_packets);
	note[TILE_TX] = BytesText(st.tx_bytes);
	value[TILE_RX].Format(_T("%llu"), st.rx_packets);
	note[TILE_RX] = BytesText(st.rx_bytes);
	value[TILE_KEEPALIVE].Format(_T("%llu / %llu"), st.ka_sent, st.ka_recv);
	note[TILE_KEEPALIVE] = _T("보냄 / 받음");
	value[TILE_DROPS].Format(_T("%llu"), m_store.TotalDrops());
	value[TILE_REHANDSHAKE].Format(_T("%llu"), st.rehandshakes);

	footer.Format(_T("패킷 %d개 표시"), m_store.ViewCount());
	for (const auto &r : reasons) {
		if (st.drops[r.verdict] != 0) {
			t.Format(_T("%s%s %llu"), note[TILE_DROPS].IsEmpty() ? _T("") : _T(", "),
					r.name, st.drops[r.verdict]);
			note[TILE_DROPS] += t;
		}
	}
	if (!note[TILE_DROPS].IsEmpty()) {
		footer += _T("   ·   폐기 사유: ") + note[TILE_DROPS];
		/* 타일은 좁다. 내역은 아래 줄에서 읽는다. */
		note[TILE_DROPS] = _T("내역은 아래 줄");
	}
	if (st.lost) {
		t.Format(_T("   ·   화면이 따라가지 못해 놓친 패킷 %llu"), st.lost);
		footer += t;
	}

	for (int i = 0; i < TILE_COUNT; i++) {
		if (value[i] != m_tile_value[i] || note[i] != m_tile_note[i]) {
			m_tile_value[i] = value[i];
			m_tile_note[i] = note[i];
			if (!m_rc_tiles[i].IsRectEmpty()) {
				InvalidateRect(&m_rc_tiles[i], FALSE);
			}
		}
	}
	if (footer != m_footer) {
		m_footer = footer;
		if (!m_rc_footer.IsRectEmpty()) {
			InvalidateRect(&m_rc_footer, FALSE);
		}
	}
}

void CMainDlg::OnTimer(UINT_PTR id)
{
	if (id == TIMER_TICK_ID) {
		UpdateStatus();		/* 경과 시간 */
		UpdateStats();

		if (++m_posture_ticks >= POSTURE_REFRESH_TICKS) {
			m_posture_ticks = 0;
			RefreshPosture(true);
		}
	}

	CDialogEx::OnTimer(id);
}

/* ---- 그리기 ------------------------------------------------------------ */

/* 라벨과 체크 상자, 읽기 전용 편집창은 스스로 그리되 바탕색을 부모에게 묻는다. 모두
 * 흰 판 위에 있으므로 판의 색을 준다. 다이얼로그의 회색이 글자 뒤에 네모로 남지 않는다. */
HBRUSH CMainDlg::OnCtlColor(CDC *pDC, CWnd *pWnd, UINT type)
{
	const ui::Palette &c = ui::Colors();

	if (type == CTLCOLOR_STATIC || type == CTLCOLOR_BTN) {
		const int id = pWnd->GetDlgCtrlID();
		const bool label = (id == IDC_LBL_ADAPTER || id == IDC_LBL_TUNIP ||
				id == IDC_LBL_SERVER || id == IDC_LBL_PORT);

		pDC->SetBkColor(c.card);
		pDC->SetTextColor(label ? c.muted : c.text);

		return static_cast<HBRUSH>(m_card_brush.GetSafeHandle());
	}

	return CDialogEx::OnCtlColor(pDC, pWnd, type);
}

/* 바탕은 OnPaint 가 한 번에 그린다. 여기서 먼저 지우면 지운 색이 잠깐 보인다. */
BOOL CMainDlg::OnEraseBkgnd(CDC *)
{
	return TRUE;
}

/* 테마가 입혀진 체크 상자 같은 컨트롤은 자기 뒤의 바탕을 부모에게 그려 달라고 한다
 * (DrawThemeParentBackground 가 WM_PRINTCLIENT 를 보낸다). 여기서 판을 그려 주지 않으면
 * 그 자리가 다이얼로그의 기본 회색으로 남는다. */
LRESULT CMainDlg::OnPrintClient(WPARAM wParam, LPARAM)
{
	PaintBackground(reinterpret_cast<HDC>(wParam));

	return 0;
}

void CMainDlg::OnPaint()
{
	CPaintDC paint(this);
	CRect client;
	HDC dc;
	HBITMAP bitmap;
	HGDIOBJ old;

	GetClientRect(&client);
	if (client.IsRectEmpty()) {
		return;
	}

	/* 화면 밖 비트맵에 다 그린 뒤 한 번에 옮긴다. 경과 시간과 통계가 초마다 바뀌는데
	 * 바로 그리면 지우고 쓰는 사이가 깜빡임으로 보인다. 컨트롤이 있는 자리는
	 * WS_CLIPCHILDREN 덕에 덮이지 않는다. */
	dc = CreateCompatibleDC(paint.GetSafeHdc());
	bitmap = CreateCompatibleBitmap(paint.GetSafeHdc(), client.Width(), client.Height());
	if (!dc || !bitmap) {
		PaintBackground(paint.GetSafeHdc());
	}
	else {
		old = SelectObject(dc, bitmap);
		PaintBackground(dc);
		BitBlt(paint.GetSafeHdc(), 0, 0, client.Width(), client.Height(), dc, 0, 0, SRCCOPY);
		SelectObject(dc, old);
	}

	if (bitmap) {
		DeleteObject(bitmap);
	}
	if (dc) {
		DeleteDC(dc);
	}
}

void CMainDlg::PaintTile(HDC dc, int tile)
{
	static const LPCTSTR labels[TILE_COUNT] = {
		_T("송신"), _T("수신"), _T("keepalive"), _T("폐기"), _T("재핸드셰이크"),
	};
	const ui::Palette &c = ui::Colors();
	const CRect &rc = m_rc_tiles[tile];
	const int pad = ui::Scale(12);
	COLORREF valueColor = c.text;
	CRect line;
	SIZE size;

	if (rc.IsRectEmpty()) {
		return;
	}

	ui::RoundRect(dc, rc, ui::Scale(6), c.card, c.border);

	/* 0 이 아닐 때만 색을 쓴다. 늘 빨간 "폐기 0" 은 문제가 없는데도 경고처럼 보인다. */
	if (tile == TILE_DROPS && m_tile_value[tile] != _T("0")) {
		valueColor = c.danger;
	}
	else if (tile == TILE_REHANDSHAKE && m_tile_value[tile] != _T("0")) {
		valueColor = c.warn;
	}

	line.SetRect(rc.left + pad, rc.top + ui::Scale(7), rc.right - pad, rc.top + rc.Height() / 2);
	ui::Text(dc, line, labels[tile], m_fonts.label, c.muted, DT_LEFT | DT_TOP | DT_SINGLELINE);

	line.SetRect(rc.left + pad, rc.top + rc.Height() / 2 - ui::Scale(4), rc.right - pad, rc.bottom - ui::Scale(6));
	ui::Text(dc, line, m_tile_value[tile], m_fonts.value, valueColor,
			DT_LEFT | DT_BOTTOM | DT_SINGLELINE);

	/* 보조 설명은 숫자 오른쪽 아래에 맞춘다. 자리가 모자라면 그리지 않는다. */
	size = ui::Measure(dc, m_tile_value[tile], m_fonts.value);
	line.left += size.cx + ui::Scale(8);
	if (!m_tile_note[tile].IsEmpty() &&
			ui::Measure(dc, m_tile_note[tile], m_fonts.label).cx <= line.Width()) {
		line.bottom -= ui::Scale(3);
		ui::Text(dc, line, m_tile_note[tile], m_fonts.label, c.muted,
				DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
	}
}

/* 컨트롤이 아닌 모든 것. 좌표는 LayoutChildren 이 정해 둔 것을 그대로 쓴다. */
void CMainDlg::PaintBackground(HDC dc)
{
	const ui::Palette &c = ui::Colors();
	const int radius = ui::Scale(8);
	CRect client;
	CRect rc;
	HBRUSH brush;

	GetClientRect(&client);
	brush = CreateSolidBrush(c.bg);
	FillRect(dc, &client, brush);
	DeleteObject(brush);

	/* 머리: 상태 */
	if (!m_rc_header.IsRectEmpty()) {
		const int dotR = ui::Scale(5);
		CRect title = m_rc_status;
		CRect sub = m_rc_status;

		ui::RoundRect(dc, m_rc_header, radius, c.card, c.border);

		title.bottom = title.top + title.Height() * 55 / 100;
		sub.top = title.bottom;

		ui::Dot(dc, title.left + dotR, title.CenterPoint().y + ui::Scale(1), dotR, m_status_dot);
		title.left += dotR * 2 + ui::Scale(9);
		sub.left = title.left;

		ui::Text(dc, title, m_status_title, m_fonts.title, c.text,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		ui::Text(dc, sub, m_status_sub, m_fonts.label, m_status_sub_color,
				DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	for (int i = 0; i < TILE_COUNT; i++) {
		PaintTile(dc, i);
	}

	/* 내용: 탭 줄 + 리스트 */
	if (!m_rc_content.IsRectEmpty()) {
		ui::RoundRect(dc, m_rc_content, radius, c.card, c.border);

		rc.SetRect(m_rc_content.left + 1, m_sep_y, m_rc_content.right - 1, m_sep_y + 1);
		brush = CreateSolidBrush(c.border);
		FillRect(dc, &rc, brush);
		DeleteObject(brush);
	}

	/* 선택한 패킷 */
	if (!m_rc_detail.IsRectEmpty()) {
		ui::RoundRect(dc, m_rc_detail, radius, c.card, c.border);

		rc.SetRect(m_rc_detail.left + ui::Scale(14), m_rc_detail.top + ui::Scale(8),
				m_rc_detail.right - ui::Scale(14), m_rc_detail.top + ui::Scale(28));
		ui::Text(dc, rc, _T("선택한 패킷"), m_fonts.strong, c.muted,
				DT_LEFT | DT_TOP | DT_SINGLELINE);
	}

	if (!m_rc_footer.IsRectEmpty()) {
		ui::Text(dc, m_rc_footer, m_footer, m_fonts.label, c.muted,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
}

/* 버튼은 ui_controls 가 그린다. */
void CMainDlg::OnDrawItem(int id, LPDRAWITEMSTRUCT dis)
{
	if (ui::DrawButton(dis)) {
		return;
	}

	CDialogEx::OnDrawItem(id, dis);
}

/* 시스템 색이나 고대비 설정이 바뀌었다. 색을 다시 정하고 전부 다시 그린다. */
void CMainDlg::OnSysColorChange()
{
	CDialogEx::OnSysColorChange();

	ui::RefreshPalette();
	m_card_brush.DeleteObject();
	m_card_brush.CreateSolidBrush(ui::Colors().card);
	ui::MatchTitleBar(GetSafeHwnd());

	m_status_title.Empty();		/* UpdateStatus 가 색을 다시 고르게 */
	UpdateStatus();
	RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- 레이아웃 ---------------------------------------------------------- */
void CMainDlg::OnGetMinMaxInfo(MINMAXINFO *mmi)
{
	CRect rc(0, 0, 520, 330);

	MapDialogRect(&rc);
	/* 클라이언트 크기 기준이라 테두리 / 캡션을 더해 준다. */
	AdjustWindowRectEx(&rc, GetStyle(), FALSE, GetExStyle());

	mmi->ptMinTrackSize.x = rc.Width();
	mmi->ptMinTrackSize.y = rc.Height();

	CDialogEx::OnGetMinMaxInfo(mmi);
}

void CMainDlg::OnSize(UINT type, int cx, int cy)
{
	CDialogEx::OnSize(type, cx, cy);

	if (type != SIZE_MINIMIZED) {
		LayoutChildren(cx, cy);
	}
}

/*  +--------------------------------------------------------------+
 *  | (●) 연결됨                 서버 [....] 포트 [..]   [ 연결 해제 ] |  머리
 *  |     00:12:31 · 세션 ....   터널 [....] 어댑터 [..]              |
 *  +--------------------------------------------------------------+
 *  [ 송신 ]   [ 수신 ]   [ keepalive ]   [ 폐기 ]   [ 재핸드셰이크 ]    타일
 *  +--------------------------------------------------------------+
 *  | 패킷  로그  포스처           [전체▾] □ □        [지움] [저장] |  내용
 *  |--------------------------------------------------------------|
 *  | 리스트                                                       |
 *  +--------------------------------------------------------------+
 *  | 선택한 패킷 (패킷 탭에서만)                                   |
 *  +--------------------------------------------------------------+
 *   패킷 N개 표시 · 폐기 사유: ...                                    바닥
 */
void CMainDlg::LayoutChildren(int cx, int cy)
{
	/* OnSize 는 컨트롤이 만들어지기 전에도 올 수 있다. */
	if (!m_packets.GetSafeHwnd() || !m_tabs) {
		return;
	}

	CRect unit(0, 0, 4, 8);
	MapDialogRect(&unit);
	const int ux = unit.Width();
	const int uy = unit.Height();

	/* 다이얼로그 단위 -> 픽셀. .rc 와 같은 숫자를 쓸 수 있어 읽기 쉽다. */
	auto DX = [ux](int du) { return du * ux / 4; };
	auto DY = [uy](int du) { return du * uy / 8; };

	const int sel = ui::TabsGetSel(m_tabs);
	const int m = DX(8);
	const int gap = DY(6);

	/* 세로 기준선: 위에서부터 머리, 타일. 아래에서부터 바닥, 상세. 남는 것이 내용이다. */
	m_rc_header.SetRect(m, DY(7), cx - m, DY(7) + DY(48));

	const int tilesTop = m_rc_header.bottom + gap;
	const int tilesH = DY(34);
	const int tileGap = DX(6);
	const int tileW = (cx - 2 * m - (TILE_COUNT - 1) * tileGap) / TILE_COUNT;

	for (int i = 0; i < TILE_COUNT; i++) {
		const int x = m + i * (tileW + tileGap);

		/* 나눗셈의 나머지는 마지막 타일이 가져가 오른쪽 끝을 맞춘다. */
		m_rc_tiles[i].SetRect(x, tilesTop, (i == TILE_COUNT - 1) ? cx - m : x + tileW, tilesTop + tilesH);
	}

	const int footerH = DY(10);
	m_rc_footer.SetRect(m + DX(4), cy - DY(4) - footerH, cx - m, cy - DY(4));

	int contentBottom = m_rc_footer.top - DY(4);

	if (sel == TAB_PACKETS) {
		const int detailH = DY(86);

		m_rc_detail.SetRect(m, contentBottom - detailH, cx - m, contentBottom);
		contentBottom = m_rc_detail.top - gap;
	}
	else {
		m_rc_detail.SetRectEmpty();
	}
	m_rc_content.SetRect(m, tilesTop + tilesH + gap, cx - m, contentBottom);

	/* 여러 컨트롤을 한 번에 옮긴다. 하나씩 MoveWindow 하면 중간 상태가 그려져 깜빡인다. */
	HDWP dwp = BeginDeferWindowPos(20);
	if (!dwp) {
		return;
	}

	auto Place = [&dwp, this](int id, int x, int y, int w, int h) {
		CWnd *c = GetDlgItem(id);
		if (c && w > 0 && h > 0) {
			dwp = DeferWindowPos(dwp, c->GetSafeHwnd(), nullptr, x, y, w, h,
					SWP_NOZORDER | SWP_NOACTIVATE);
		}
	};

	/* 머리: 오른쪽 끝에 버튼, 그 왼쪽에 입력 칸 두 줄, 남는 왼쪽이 상태다. */
	const int btnW = DX(74);
	const int btnH = DY(20);
	const int btnX = m_rc_header.right - DX(10) - btnW;
	Place(IDC_CONNECT, btnX, m_rc_header.top + (m_rc_header.Height() - btnH) / 2, btnW, btnH);

	const int lblW = DX(30);
	const int edit1W = DX(88);
	const int edit2W = DX(60);
	const int edit2X = btnX - DX(14) - edit2W;
	const int lbl2X = edit2X - lblW;
	const int edit1X = lbl2X - DX(10) - edit1W;
	const int lbl1X = edit1X - lblW;
	const int row1 = m_rc_header.top + DY(9);
	const int row2 = m_rc_header.top + DY(27);
	const int editH = DY(13);

	Place(IDC_LBL_SERVER, lbl1X, row1 + DY(2), lblW, DY(9));
	Place(IDC_SERVER, edit1X, row1, edit1W, editH);
	Place(IDC_LBL_PORT, lbl2X, row1 + DY(2), lblW, DY(9));
	Place(IDC_PORT, edit2X, row1, edit2W, editH);
	Place(IDC_LBL_TUNIP, lbl1X, row2 + DY(2), lblW, DY(9));
	Place(IDC_TUNIP, edit1X, row2, edit1W, editH);
	Place(IDC_LBL_ADAPTER, lbl2X, row2 + DY(2), lblW, DY(9));
	Place(IDC_ADAPTER, edit2X, row2, edit2W, editH);

	m_rc_status.SetRect(m_rc_header.left + DX(12), m_rc_header.top + DY(8),
			lbl1X - DX(8), m_rc_header.bottom - DY(8));

	/* 내용의 윗줄: 왼쪽에 탭, 오른쪽에 그 탭의 도구 */
	const int rowTop = m_rc_content.top + DY(3);
	const int rowH = DY(18);
	const int toolH = DY(14);
	const int toolY = rowTop + (rowH - toolH) / 2;
	int tabsW = ui::TabsIdealWidth(m_tabs);
	int x = m_rc_content.right - DX(8);

	if (tabsW <= 0) {
		tabsW = DX(150);
	}
	Place(IDC_TAB, m_rc_content.left + DX(4), rowTop, tabsW, rowH);
	m_sep_y = rowTop + rowH + DY(2);

	/* 기기 등록은 포스처 탭에서, 나머지는 패킷 / 로그 탭에서 같은 자리를 쓴다. */
	Place(IDC_ENROLL, x - DX(64), toolY, DX(64), toolH);

	x -= DX(42);
	Place(IDC_SAVE, x, toolY, DX(42), toolH);
	if (sel != TAB_PACKETS) {
		x += DX(42);	/* 저장이 없는 탭에서는 지움이 끝으로 온다 */
	}
	else {
		x -= DX(4);
	}
	x -= DX(36);
	Place(IDC_CLEAR, x, toolY, DX(36), toolH);

	x -= DX(12) + DX(44);
	Place(IDC_ONLY_DROPS, x, rowTop + (rowH - DY(10)) / 2, DX(44), DY(10));
	x -= DX(4) + DX(76);
	Place(IDC_HIDE_KA, x, rowTop + (rowH - DY(10)) / 2, DX(76), DY(10));
	x -= DX(8) + DX(52);
	/* 콤보 상자의 높이는 펼쳤을 때의 높이다. */
	Place(IDC_FILTER_DIR, x, rowTop + (rowH - DY(13)) / 2, DX(52), DY(80));

	/* 리스트는 판의 둥근 모서리를 덮지 않게 조금 들여 넣는다. */
	const int listX = m_rc_content.left + DX(2);
	const int listY = m_sep_y + 1;
	const int listW = m_rc_content.Width() - 2 * DX(2);
	const int listH = m_rc_content.bottom - DY(4) - listY;

	Place(IDC_PACKETS, listX, listY, listW, listH);
	Place(IDC_LOG, listX + DX(4), listY + DY(2), listW - DX(4), listH - DY(2));
	Place(IDC_POSTURE, listX, listY, listW, listH);

	if (!m_rc_detail.IsRectEmpty()) {
		Place(IDC_DETAIL, m_rc_detail.left + DX(8), m_rc_detail.top + DY(16),
				m_rc_detail.Width() - 2 * DX(8), m_rc_detail.Height() - DY(20));
	}

	EndDeferWindowPos(dwp);

	/* 판의 자리가 바뀌었으니 바탕을 다시 그린다. */
	Invalidate(FALSE);
}

/* ---- 종료 -------------------------------------------------------------- */
void CMainDlg::OnDestroy()
{
	KillTimer(TIMER_TICK_ID);
	SaveSettings();

	/* 순서가 중요하다.
	 *  1) destroy 가 종료를 요청하고 워커가 끝날 때까지 기다린다. 이 시점 이후로는
	 *     새 PostMessage 가 생기지 않는다.
	 *  2) 그 뒤에 큐에 남아 있는 WM_VPN_LOG 의 버퍼를 해제한다. 창이 사라지면
	 *     이 메시지들은 처리되지 않으므로 여기서 치우지 않으면 그대로 샌다.
	 * ESC / Enter 로 닫을 때는 WM_CLOSE 없이 바로 WM_DESTROY 가 오므로
	 * 이 처리를 OnClose 가 아니라 여기에 둔다. */
	vpn_session_destroy(m_session);
	m_session = nullptr;
	vpn_session_drain(GetSafeHwnd());

	CDialogEx::OnDestroy();

	/* 컨트롤이 쓰던 글꼴은 컨트롤이 사라진 뒤에 지운다. */
	ui::DestroyFonts(&m_fonts);
}
