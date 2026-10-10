#include "framework.h"
#include "resource.h"
#include "MainDlg.h"

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
	, m_state_color(GetSysColor(COLOR_WINDOWTEXT))
{
}

void CMainDlg::DoDataExchange(CDataExchange *pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PACKETS, m_packets);
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
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB, &CMainDlg::OnTabChanged)
	ON_MESSAGE(WM_VPN_LOG, &CMainDlg::OnVpnLog)
	ON_MESSAGE(WM_VPN_STATE, &CMainDlg::OnVpnState)
	ON_MESSAGE(WM_VPN_DONE, &CMainDlg::OnVpnDone)
	ON_MESSAGE(WM_VPN_PACKETS, &CMainDlg::OnVpnPackets)
END_MESSAGE_MAP()

BOOL CMainDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	m_tab.InsertItem(0, _T("패킷"));
	m_tab.InsertItem(1, _T("로그"));

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
		}
	} while (n == PACKET_DRAIN_BATCH);

	m_store.SetLost(vpn_session_lost_packets(m_session));
	RefreshPacketCount();

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
	BOOL showPackets = (sel != 1);

	m_packets.ShowWindow(showPackets ? SW_SHOW : SW_HIDE);
	m_log.ShowWindow(showPackets ? SW_HIDE : SW_SHOW);

	if (pResult) {
		*pResult = 0;
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
	default:			return _T("-");
	}
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
		if (m_connected_at) {
			ULONGLONG sec = (GetTickCount64() - m_connected_at) / 1000;
			CString s;
			s.Format(_T("%02llu:%02llu:%02llu"), sec / 3600, (sec / 60) % 60, sec % 60);
			SetDlgItemText(IDC_ELAPSED, s);
		}
		UpdateStats();
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
