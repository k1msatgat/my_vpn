#include "framework.h"
#include "resource.h"
#include "MainDlg.h"
#include "device.h"

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

/* 행 배경색. 방향과 폐기 여부가 한눈에 들어오게만 하고 과하게 칠하지 않는다. */
#define COLOR_TX_BG	RGB(240, 247, 255)
#define COLOR_RX_BG	RGB(242, 251, 243)
#define COLOR_DROP_BG	RGB(255, 241, 241)
#define COLOR_DROP_FG	RGB(176, 32, 32)

CMainDlg::CMainDlg(CWnd *parent)
	: CDialogEx(IDD_MAIN, parent)
	, m_session(nullptr)
	, m_state(TUNNEL_STATE_STOPPED)
	, m_connected_at(0)
	, m_has_reported(false)
	, m_deny_reason(0)
	, m_posture_ticks(0)
	, m_state_color(GetSysColor(COLOR_WINDOWTEXT))
{
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
	DDX_Control(pDX, IDC_TAB, m_tab);
	DDX_Control(pDX, IDC_DETAIL, m_detail);
}

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
	ON_BN_CLICKED(IDC_CONNECT, &CMainDlg::OnConnect)
	ON_BN_CLICKED(IDC_DISCONNECT, &CMainDlg::OnDisconnect)
	ON_BN_CLICKED(IDC_CLEAR, &CMainDlg::OnClear)
	ON_BN_CLICKED(IDC_SAVE, &CMainDlg::OnSave)
	ON_BN_CLICKED(IDC_HIDE_KA, &CMainDlg::OnFilterChanged)
	ON_BN_CLICKED(IDC_ONLY_DROPS, &CMainDlg::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_FILTER_DIR, &CMainDlg::OnFilterChanged)
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_WM_TIMER()
	ON_WM_CTLCOLOR()
	ON_NOTIFY(LVN_GETDISPINFO, IDC_PACKETS, &CMainDlg::OnGetDispInfo)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PACKETS, &CMainDlg::OnPacketCustomDraw)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_PACKETS, &CMainDlg::OnPacketSelChanged)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_POSTURE, &CMainDlg::OnPostureCustomDraw)
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB, &CMainDlg::OnTabChanged)
	ON_MESSAGE(WM_VPN_LOG, &CMainDlg::OnVpnLog)
	ON_MESSAGE(WM_VPN_STATE, &CMainDlg::OnVpnState)
	ON_MESSAGE(WM_VPN_DONE, &CMainDlg::OnVpnDone)
	ON_MESSAGE(WM_VPN_PACKETS, &CMainDlg::OnVpnPackets)
END_MESSAGE_MAP()

BOOL CMainDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	m_tab.InsertItem(TAB_PACKETS, _T("패킷"));
	m_tab.InsertItem(TAB_LOG, _T("로그"));
	m_tab.InsertItem(TAB_POSTURE, _T("포스처"));

	CComboBox *filter = (CComboBox *)GetDlgItem(IDC_FILTER_DIR);
	filter->AddString(_T("전체"));
	filter->AddString(_T("송신"));
	filter->AddString(_T("수신"));
	filter->SetCurSel(PKT_FILTER_ALL);

	SetupPacketColumns();

	/* 가상 리스트: 행을 컨트롤에 복사하지 않고 그릴 때마다 LVN_GETDISPINFO 로
	 * 물어본다. 수만 행이 되어도 컨트롤 쪽 메모리와 삽입 비용이 늘지 않는다.
	 * DOUBLEBUFFER 는 빠르게 추가될 때의 깜빡임을 없앤다. */
	m_packets.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
			LVS_EX_HEADERDRAGDROP);

	m_mono.CreatePointFont(90, _T("Consolas"));
	m_detail.SetFont(&m_mono);

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

	CRect rc;
	GetClientRect(&rc);
	LayoutChildren(rc.Width(), rc.Height());
	OnTabChanged(nullptr, nullptr);

	return TRUE;
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
		AfxMessageBox(_T("어댑터 이름을 확인하세요."), MB_ICONWARNING);
		return false;
	}
	/* adapter_name 은 wchar_t 배열이고 cfg 는 구조체째로 복사되므로
	 * CString 의 수명에 의존하지 않는다. */
	wcscpy_s(cfg.adapter_name, TUNNEL_ADAPTER_NAME_LEN, adapter);

	if (InetPtonW(AF_INET, server, &cfg.server_ip) != 1) {
		AfxMessageBox(_T("서버 IP 를 확인하세요."), MB_ICONWARNING);
		return false;
	}

	port_num = _ttoi(port);
	if (port_num <= 0 || port_num > 65535) {
		AfxMessageBox(_T("포트를 확인하세요 (1-65535)."), MB_ICONWARNING);
		return false;
	}
	cfg.server_port = static_cast<uint16_t>(port_num);

	if (InetPtonW(AF_INET, tun_ip, &cfg.tun_ip) != 1) {
		AfxMessageBox(_T("터널 IP 를 확인하세요."), MB_ICONWARNING);
		return false;
	}

	/* 기기 신원. 등록은 아직 콘솔에서만 할 수 있다. */
	device_t dev;
	int enrolled = device_load(&dev);

	if (enrolled == DEVICE_NOT_ENROLLED) {
		AfxMessageBox(_T("등록되지 않은 기기입니다.\n\n")
				_T("명령 프롬프트에서 vpn_client enroll 을 실행하고,\n")
				_T("출력된 device id 와 key 를 서버에 등록하세요."), MB_ICONWARNING);
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
void CMainDlg::OnConnect()
{
	tunnel_config_t cfg;

	if (vpn_session_active(m_session)) {
		return;
	}

	if (!BuildConfig(cfg)) {
		return;
	}

	SaveSettings();

	/* 지난 연결의 판정과 보고 값은 새 연결과 무관하다. */
	m_deny_reason = 0;
	m_has_reported = false;
	SetDlgItemText(IDC_ELAPSED, _T(""));

	if (vpn_session_start(m_session, &cfg) < 0) {
		AppendLog(_T("[gui] 워커 스레드를 만들 수 없습니다"));
		return;
	}

	/* tunnel_run 도 곧 CONNECTING 을 보내지만, 버튼을 누른 즉시 반응을 보이도록
	 * 여기서 한 번 더 바꿔 준다. */
	SetUiState(TUNNEL_STATE_CONNECTING);
}

void CMainDlg::OnDisconnect()
{
	if (!vpn_session_active(m_session)) {
		return;
	}

	AppendLog(_T("[gui] 해제 요청"));
	vpn_session_stop(m_session);

	/* 여기서 워커를 기다리지 않는다. 기다리면 UI 가 멈추고, 그 사이 워커가 보낸
	 * PostMessage 도 처리되지 않는다. 워커가 끝나면 WM_VPN_DONE 이 오고 거기서 join 한다. */
	GetDlgItem(IDC_DISCONNECT)->EnableWindow(FALSE);
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
	if (m_deny_reason != 0) {
		SetDlgItemText(IDC_ELAPSED, DenyReasonName(m_deny_reason));
	}
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

void CMainDlg::OnPacketCustomDraw(NMHDR *pNMHDR, LRESULT *pResult)
{
	LPNMLVCUSTOMDRAW cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(pNMHDR);

	switch (cd->nmcd.dwDrawStage) {
	case CDDS_PREPAINT:
		*pResult = CDRF_NOTIFYITEMDRAW;
		return;

	case CDDS_ITEMPREPAINT: {
		const tunnel_packet_t *p = m_store.ViewAt((int)cd->nmcd.dwItemSpec);

		if (p) {
			if (p->verdict != TUNNEL_PASS) {
				cd->clrTextBk = COLOR_DROP_BG;
				cd->clrText = COLOR_DROP_FG;
			}
			else {
				cd->clrTextBk = (p->dir == TUNNEL_DIR_TX) ? COLOR_TX_BG : COLOR_RX_BG;
				cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
			}
		}
		*pResult = CDRF_DODEFAULT;
		return;
	}
	default:
		*pResult = CDRF_DODEFAULT;
		return;
	}
}

void CMainDlg::OnPacketSelChanged(NMHDR *pNMHDR, LRESULT *pResult)
{
	NMLISTVIEW *lv = reinterpret_cast<NMLISTVIEW *>(pNMHDR);

	*pResult = 0;

	if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) {
		ShowDetail(lv->iItem);
	}
}

void CMainDlg::OnTabChanged(NMHDR *, LRESULT *pResult)
{
	int sel = m_tab.GetCurSel();

	m_packets.ShowWindow(sel == TAB_PACKETS ? SW_SHOW : SW_HIDE);
	m_log.ShowWindow(sel == TAB_LOG ? SW_SHOW : SW_HIDE);
	m_posture.ShowWindow(sel == TAB_POSTURE ? SW_SHOW : SW_HIDE);

	if (sel == TAB_POSTURE) {
		RefreshPosture(true);
	}

	if (pResult) {
		*pResult = 0;
	}
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
	m_posture.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

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

		if (row >= 0 && row < POSTURE_ROW_COUNT && m_posture_bad[row]) {
			cd->clrTextBk = COLOR_DROP_BG;
			cd->clrText = COLOR_DROP_FG;
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
	LPCTSTR text;
	BOOL stopped;

	switch (state) {
	case TUNNEL_STATE_CONNECTING:
		text = _T("연결 중");
		m_state_color = RGB(176, 108, 0);
		break;
	case TUNNEL_STATE_CONNECTED:
		text = _T("연결됨");
		m_state_color = RGB(16, 124, 16);
		break;
	case TUNNEL_STATE_RECONNECTING:
		text = _T("재연결 중");
		m_state_color = RGB(176, 32, 32);
		break;
	case TUNNEL_STATE_DENIED:
		text = _T("접근 거부됨");
		m_state_color = RGB(176, 32, 32);
		break;
	case TUNNEL_STATE_STOPPED:
	default:
		text = _T("중지됨");
		m_state_color = GetSysColor(COLOR_GRAYTEXT);
		break;
	}

	if (state == TUNNEL_STATE_CONNECTED && m_connected_at == 0) {
		m_connected_at = GetTickCount64();
	}
	else if (state == TUNNEL_STATE_STOPPED) {
		m_connected_at = 0;
		SetDlgItemText(IDC_ELAPSED, _T(""));
	}

	m_state = state;
	SetDlgItemText(IDC_STATE, text);
	GetDlgItem(IDC_STATE)->Invalidate();

	/* 거부됐으면 상태 아래 줄에 사유를 둔다. 탭을 옮기지 않아도 왜 막혔는지 보인다.
	 * 연결이 끊긴 뒤에도 남겨 둔다 — 첫 핸드셰이크에서 거부되면 곧바로 "중지됨"이
	 * 되므로, 지우면 사유를 볼 틈이 없다. */
	if (m_deny_reason != 0) {
		SetDlgItemText(IDC_ELAPSED, DenyReasonName(m_deny_reason));
	}
	RefreshPosture(false);

	stopped = (state == TUNNEL_STATE_STOPPED) ? TRUE : FALSE;

	GetDlgItem(IDC_CONNECT)->EnableWindow(stopped);
	GetDlgItem(IDC_DISCONNECT)->EnableWindow(!stopped);

	/* 연결 중에 설정을 바꿔도 반영되지 않으므로 잠근다. */
	GetDlgItem(IDC_ADAPTER)->EnableWindow(stopped);
	GetDlgItem(IDC_SERVER)->EnableWindow(stopped);
	GetDlgItem(IDC_PORT)->EnableWindow(stopped);
	GetDlgItem(IDC_TUNIP)->EnableWindow(stopped);
}

void CMainDlg::UpdateStats()
{
	const PacketStats &st = m_store.Stats();
	CString s;

	s.Format(_T("송신 %llu (%llu B)    수신 %llu (%llu B)    keepalive %llu / %llu    보관 %d / 표시 %d"),
			st.tx_packets, st.tx_bytes, st.rx_packets, st.rx_bytes,
			st.ka_sent, st.ka_recv, m_store.ViewCount(), m_store.ViewCount());
	SetDlgItemText(IDC_STATS1, s);

	s.Format(_T("폐기 %llu  —  비유니캐스트 %llu, 세션 불일치 %llu, 서버 아닌 출발지 %llu, 헤더 불량 %llu, IPv4 아님 %llu    재핸드셰이크 %llu"),
			m_store.TotalDrops(),
			st.drops[TUNNEL_DROP_NON_UNICAST],
			st.drops[TUNNEL_DROP_WRONG_SESSION],
			st.drops[TUNNEL_DROP_NOT_SERVER],
			st.drops[TUNNEL_DROP_BAD_HEADER],
			st.drops[TUNNEL_DROP_NON_IPV4],
			st.rehandshakes);

	if (st.lost) {
		CString t;
		t.Format(_T("    [UI 유실 %llu]"), st.lost);
		s += t;
	}
	SetDlgItemText(IDC_STATS2, s);
}

void CMainDlg::OnTimer(UINT_PTR id)
{
	if (id == TIMER_TICK_ID) {
		/* 거부 중에는 그 줄에 사유가 떠 있으므로 경과 시간으로 덮지 않는다. */
		if (m_connected_at && m_deny_reason == 0) {
			ULONGLONG sec = (GetTickCount64() - m_connected_at) / 1000;
			CString s;
			s.Format(_T("%02llu:%02llu:%02llu"), sec / 3600, (sec / 60) % 60, sec % 60);
			SetDlgItemText(IDC_ELAPSED, s);
		}
		UpdateStats();

		if (++m_posture_ticks >= POSTURE_REFRESH_TICKS) {
			m_posture_ticks = 0;
			RefreshPosture(true);
		}
	}

	CDialogEx::OnTimer(id);
}

HBRUSH CMainDlg::OnCtlColor(CDC *pDC, CWnd *pWnd, UINT type)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, type);

	if (pWnd->GetDlgCtrlID() == IDC_STATE) {
		pDC->SetTextColor(m_state_color);
	}

	return hbr;
}

/* ---- 레이아웃 ---------------------------------------------------------- */
void CMainDlg::OnGetMinMaxInfo(MINMAXINFO *mmi)
{
	CRect rc(0, 0, 420, 320);

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

void CMainDlg::LayoutChildren(int cx, int cy)
{
	/* OnSize 는 컨트롤이 만들어지기 전에도 올 수 있다. */
	if (!m_packets.GetSafeHwnd() || !m_tab.GetSafeHwnd()) {
		return;
	}

	CRect unit(0, 0, 4, 8);
	MapDialogRect(&unit);
	const int ux = unit.Width();
	const int uy = unit.Height();

	/* 다이얼로그 단위 -> 픽셀. .rc 와 같은 숫자를 쓸 수 있어 읽기 쉽다. */
	auto DX = [ux](int du) { return du * ux / 4; };
	auto DY = [uy](int du) { return du * uy / 8; };

	const int m = DX(7);
	const int btnW = DX(76);
	const int btnH = DY(18);

	/* 세로 기준선 */
	const int connTop = DY(4);
	const int connH = DY(62);
	const int filterTop = connTop + connH + DY(6);
	const int filterH = DY(16);
	const int tabTop = filterTop + filterH + DY(4);

	const int statsH = DY(11);
	const int stats2Top = cy - DY(4) - statsH;
	const int stats1Top = stats2Top - statsH;
	const int detailBottom = stats1Top - DY(4);
	const int detailH = DY(96);
	const int detailTop = detailBottom - detailH;
	const int tabBottom = detailTop - DY(6);

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

	/* 연결 그룹: 버튼과 상태는 오른쪽에 붙인다 */
	Place(IDC_GRP_CONN, m, connTop, cx - 2 * m, connH);

	const int btnX = cx - m - DX(8) - btnW;
	Place(IDC_CONNECT, btnX, connTop + DY(12), btnW, btnH);
	Place(IDC_DISCONNECT, btnX, connTop + DY(34), btnW, btnH);

	const int stLblX = btnX - DX(142);
	Place(IDC_LBL_STATE, stLblX, connTop + DY(16), DX(28), DY(9));
	Place(IDC_STATE, stLblX + DX(30), connTop + DY(16), DX(100), DY(9));
	Place(IDC_ELAPSED, stLblX + DX(30), connTop + DY(36), DX(100), DY(9));

	/* 필터 줄: 지움 / 저장은 오른쪽 */
	const int toolW = DX(60);
	Place(IDC_SAVE, cx - m - toolW, filterTop, toolW, filterH);
	Place(IDC_CLEAR, cx - m - 2 * toolW - DX(8), filterTop, toolW, filterH);

	/* 탭과 그 안의 리스트 / 로그 */
	const int tabH = tabBottom - tabTop;
	Place(IDC_TAB, m, tabTop, cx - 2 * m, tabH);

	CRect inner(m, tabTop, cx - m, tabTop + tabH);
	m_tab.AdjustRect(FALSE, &inner);
	inner.DeflateRect(DX(2), DY(1));
	Place(IDC_PACKETS, inner.left, inner.top, inner.Width(), inner.Height());
	Place(IDC_LOG, inner.left, inner.top, inner.Width(), inner.Height());
	Place(IDC_POSTURE, inner.left, inner.top, inner.Width(), inner.Height());

	/* 상세창 */
	Place(IDC_GRP_DETAIL, m, detailTop, cx - 2 * m, detailH);
	Place(IDC_DETAIL, m + DX(6), detailTop + DY(12), cx - 2 * m - DX(12), detailH - DY(18));

	/* 통계 두 줄 */
	Place(IDC_STATS1, m + DX(3), stats1Top, cx - 2 * m, statsH);
	Place(IDC_STATS2, m + DX(3), stats2Top, cx - 2 * m, statsH);

	EndDeferWindowPos(dwp);
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
}
