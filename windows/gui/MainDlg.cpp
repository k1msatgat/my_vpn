#include "framework.h"
#include <uxtheme.h>
#include "resource.h"
#include "MainDlg.h"
#include "EnrollDlg.h"
#include "PacketText.h"
#include "DiagDlg.h"

#ifdef _MSC_VER
#pragma comment(lib, "uxtheme.lib")
#endif


CMainDlg::CMainDlg(CWnd *parent)
	: CDialogEx(IDD_MAIN, parent)
	, m_session(nullptr)
	, m_state(TUNNEL_STATE_STOPPED)
	, m_connected_at(0)
	, m_has_reported(false)
	, m_deny_reason(0)
	, m_posture_ticks(0)
	, m_session_idx(0)
	, m_link(VPN_REMOTE_DOWN)
	, m_stopping(false)
	, m_diag(m_store, m_logLines)
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
	DDX_Control(pDX, IDC_POSTURE, m_posture);
}

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
	ON_BN_CLICKED(IDC_CONNECT, &CMainDlg::OnConnect)
	ON_BN_CLICKED(IDC_ENROLL, &CMainDlg::OnEnroll)
	ON_BN_CLICKED(IDC_DIAG, &CMainDlg::OnDiag)
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
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_POSTURE, &CMainDlg::OnPostureCustomDraw)
	ON_MESSAGE(WM_VPN_LOG, &CMainDlg::OnVpnLog)
	ON_MESSAGE(WM_VPN_STATE, &CMainDlg::OnVpnState)
	ON_MESSAGE(WM_VPN_DONE, &CMainDlg::OnVpnDone)
	ON_MESSAGE(WM_VPN_PACKETS, &CMainDlg::OnVpnPackets)
	ON_MESSAGE(WM_VPN_SERVICE, &CMainDlg::OnVpnService)
	ON_MESSAGE(WM_VPN_DEVICE, &CMainDlg::OnVpnDevice)
	ON_MESSAGE(WM_VPN_ERROR, &CMainDlg::OnVpnError)
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

	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_CONNECT), ui::BUTTON_PRIMARY);
	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_ENROLL), ui::BUTTON_SECONDARY);
	ui::StyleButton(::GetDlgItem(GetSafeHwnd(), IDC_DIAG), ui::BUTTON_SECONDARY);

	/* 빈 칸에 무엇을 적는지 흐린 글씨로 알려 준다. */
	GetDlgItem(IDC_SERVER)->SendMessage(EM_SETCUEBANNER, FALSE, (LPARAM)L"203.0.113.10");
	GetDlgItem(IDC_PORT)->SendMessage(EM_SETCUEBANNER, FALSE, (LPARAM)L"9000");
	/* 터널 IP 는 적는 칸이 아니라 보는 칸이다. 서버가 이 기기에 정해 준 주소가 연결된
	 * 뒤에 여기에 나온다. 전에는 서버가 이미 아는 값을 사용자가 똑같이 한 번 더 적어야
	 * 했고, 틀리면 거절당할 뿐이었다. */
	GetDlgItem(IDC_TUNIP)->SendMessage(EM_SETREADONLY, TRUE, 0);
	GetDlgItem(IDC_TUNIP)->SendMessage(EM_SETCUEBANNER, TRUE, (LPARAM)L"서버가 배정");

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
	SetTimer(TIMER_TICK_ID, TIMER_TICK_MS, nullptr);

	/* 첫 WM_SIZE 는 컨트롤이 붙기 전에 와서 LayoutChildren 이 그냥 돌아간다. 여기서 한 번
	 * 자리를 잡아 준다. 안 그러면 다음에 창 크기를 바꿀 때까지 .rc 의 초깃값 자리에 있고
	 * 판도 그려지지 않는다. */
	CRect rc;
	GetClientRect(&rc);
	LayoutChildren(rc.Width(), rc.Height());

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

/* ---- 설정 저장 / 복원 --------------------------------------------------- */
void CMainDlg::LoadSettings()
{
	CWinApp *app = AfxGetApp();

	SetDlgItemText(IDC_ADAPTER, app->GetProfileString(_T("conn"), _T("adapter"), _T("ztgate")));
	SetDlgItemText(IDC_SERVER, app->GetProfileString(_T("conn"), _T("server"), _T("49.247.139.39")));
	SetDlgItemText(IDC_PORT, app->GetProfileString(_T("conn"), _T("port"), _T("9000")));
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
}

/* ---- 설정 읽기 ---------------------------------------------------------- */
bool CMainDlg::BuildConfig(tunnel_config_t &cfg)
{
	CString adapter;
	CString server;
	CString port;
	int port_num;

	GetDlgItemText(IDC_ADAPTER, adapter);
	GetDlgItemText(IDC_SERVER, server);
	GetDlgItemText(IDC_PORT, port);

	adapter.Trim();
	server.Trim();
	port.Trim();

	ZeroMemory(&cfg, sizeof(cfg));
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

	/* 터널은 서비스가 연다. 이 프로그램은 어디로 붙을지만 알려 준다 — 기기의 키는
	 * 서비스만 읽을 수 있는 곳에 있고 여기로는 오지 않는다. */
	if (!ServiceReady()) {
		return false;
	}

	if (m_device_id.IsEmpty()) {
		AfxMessageBox(_T("등록되지 않은 기기입니다.\n\n")
				_T("[기기 등록...] 에 관리자에게 받은 등록 코드를 적어 주세요."), MB_ICONWARNING);
		/* 어디로 가야 하는지 말로만 하지 않고 그 버튼으로 데려다 준다. */
		GotoDlgCtrl(GetDlgItem(IDC_ENROLL));
		return false;
	}

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

/* 이 기기의 ID 와 키를 새로 만든다. */
void CMainDlg::OnEnroll()
{
	device_t dev;

	if (!ServiceReady()) {
		return;
	}

	/* 열려 있는 터널은 예전 키로 인증한 세션이다. 지금 키를 바꾸면 화면의 기기 ID 와
	 * 실제로 쓰이는 키가 어긋난다. 서비스도 터널이 열려 있는 동안에는 받지 않는다. */
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

	/* 보통은 관리자가 준 등록 코드로 올린다. 어느 서버에 올리는지는 위의 연결 설정을
	 * 그대로 쓴다 — 등록한 서버와 연결할 서버가 다를 이유가 없다. */
	CString server;
	CString port;

	GetDlgItemText(IDC_SERVER, server);
	GetDlgItemText(IDC_PORT, port);
	server.Trim();
	port.Trim();

	CEnrollCodeDlg ask(_T("올릴 서버  ") + server + _T(" : ") + port, this);
	INT_PTR choice = ask.DoModal();

	if (choice == CEnrollCodeDlg::RESULT_MANUAL) {
		/* 코드 없이: 서비스가 ID 와 키를 만들어 저장하고 한 번 돌려준다. 그것을 보여
		 * 주고, 관리자가 손으로 서버에 옮겨 적는다. */
		{
			CWaitCursor wait;

			if (vpn_remote_enroll_manual(m_session, &dev) != DEVICE_OK) {
				AfxMessageBox(_T("기기 등록에 실패했습니다."), MB_ICONERROR);
				return;
			}
		}

		{
			CEnrollDlg dlg(dev, this);

			SecureZeroMemory(&dev, sizeof(dev));
			dlg.DoModal();
		}

		LoadDeviceId();
		RefreshPosture(false);
		AppendLog(_T("[gui] 기기 등록 (수동): ") + m_device_id);
		return;
	}
	if (choice != IDOK) {
		return;
	}

	tunnel_config_t cfg;
	int port_num = _ttoi(port);

	ZeroMemory(&cfg, sizeof(cfg));
	if (InetPtonW(AF_INET, server, &cfg.server_ip) != 1) {
		FieldError(IDC_SERVER, _T("등록할 서버의 IPv4 주소를 먼저 적어 주세요. 예) 203.0.113.10"));
		return;
	}
	if (port_num <= 0 || port_num > 65535) {
		FieldError(IDC_PORT, _T("1 부터 65535 사이의 숫자여야 합니다."));
		return;
	}
	cfg.server_port = static_cast<uint16_t>(port_num);

	int result;

	{
		/* 서비스가 서버와 주고받는 동안 창이 멈춘다. 길어야 십여 초이고, 그동안 할 수
		 * 있는 일도 없다. 신원을 만들고 저장하는 것은 서비스다 — 키는 이 프로그램을
		 * 지나가지 않는다. */
		CWaitCursor wait;
		CStringA code(ask.m_code);

		result = vpn_remote_enroll(m_session, cfg.server_ip.s_addr, cfg.server_port, code);
	}

	const bool saved = true;	/* 저장은 서비스가 했다. 못 했으면 result 가 OK 가 아니다 */

	switch (result) {
	case TUNNEL_ENROLL_OK:
		if (!saved) {
			AfxMessageBox(_T("서버에는 등록됐지만 이 PC 에 신원을 저장하지 못했습니다.\n\n")
					_T("관리자에게 그 기기를 지우고 새 코드를 달라고 하세요."), MB_ICONERROR);
			return;
		}
		break;
	case TUNNEL_ENROLL_BAD_CODE:
		AfxMessageBox(_T("등록 코드가 아닙니다. 글자와 숫자 25자입니다."), MB_ICONWARNING);
		return;
	case TUNNEL_ENROLL_NO_RESPONSE:
		AfxMessageBox(_T("서버가 답하지 않습니다. 서버 주소와 포트를 확인하세요."), MB_ICONWARNING);
		return;
	case TUNNEL_ENROLL_REJECTED:
		AfxMessageBox(_T("서버가 이 코드를 받지 않습니다.\n\n")
				_T("잘못 적었거나, 만료됐거나, 이미 쓰인 코드입니다."), MB_ICONWARNING);
		return;
	case TUNNEL_ENROLL_CONFLICT:
		AfxMessageBox(_T("이 기기나 이 코드에 묶인 터널 주소가 이미 서버에 있습니다.\n\n")
				_T("관리자에게 새 코드를 달라고 하세요."), MB_ICONWARNING);
		return;
	case TUNNEL_ENROLL_SERVER_FAILED:
		AfxMessageBox(_T("서버가 기기를 저장하지 못했습니다. 관리자에게 알려 주세요."), MB_ICONERROR);
		return;
	default:
		AfxMessageBox(_T("기기 등록에 실패했습니다."), MB_ICONERROR);
		return;
	}

	LoadDeviceId();
	RefreshPosture(false);
	AppendLog(_T("[gui] 기기 등록: ") + m_device_id);
	AfxMessageBox(_T("등록됐습니다. 이제 [연결]을 누르면 됩니다."), MB_ICONINFORMATION);
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
	m_diag.PacketsChanged();

	/* 상태 변화(WM_VPN_STATE)와 패킷(WM_VPN_PACKETS)은 따로 오는 메시지라, "거부됨"
	 * 상태가 DENY 패킷보다 먼저 처리될 수 있다. 그때는 SetUiState 가 사유를 아직
	 * 모르므로 사유를 알게 된 여기서도 한 번 더 쓴다. */
	UpdateStatus();
	RefreshPosture(false);

	return 0;
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
	uint8_t id[DEVICE_ID_LEN];

	m_device_id.Empty();

	/* 신원은 서비스가 갖고 있다. 서비스에 붙기 전에는 등록됐는지조차 알 수 없다. */
	if (vpn_remote_device(m_session, id)) {
		CString t;
		for (int i = 0; i < DEVICE_ID_LEN; i++) {
			t.Format(_T("%02x"), id[i]);
			m_device_id += t;
		}
	}
}

/* 서비스에 붙어 있는가. 아니면 왜 안 되는지와 무엇을 해야 하는지를 알려 준다. */
bool CMainDlg::ServiceReady()
{
	switch (m_link) {
	case VPN_REMOTE_UP:
		return true;
	case VPN_REMOTE_MISMATCH:
		AfxMessageBox(_T("서비스가 이 프로그램과 다른 버전입니다.\n\n")
				_T("같이 빌드된 vpn_service.exe 로 다시 설치하세요:\n")
				_T("관리자 명령 프롬프트에서  vpn_service.exe install"), MB_ICONWARNING);
		return false;
	case VPN_REMOTE_BUSY:
		AfxMessageBox(_T("다른 프로그램이 이미 서비스에 붙어 있습니다.\n\n")
				_T("이 창이 하나 더 떠 있는지 확인하세요."), MB_ICONWARNING);
		return false;
	default:
		AfxMessageBox(_T("터널을 맡는 서비스가 돌고 있지 않습니다.\n\n")
				_T("한 번 설치하면 됩니다. 관리자 명령 프롬프트에서:\n")
				_T("vpn_service.exe install"), MB_ICONWARNING);
		return false;
	}
}

/* 서비스와의 연결이 바뀌었다. */
LRESULT CMainDlg::OnVpnService(WPARAM wParam, LPARAM)
{
	m_link = static_cast<int>(wParam);

	switch (m_link) {
	case VPN_REMOTE_UP:
		AppendLog(_T("[gui] 서비스에 붙었습니다"));
		break;
	case VPN_REMOTE_MISMATCH:
		AppendLog(_T("[gui] 서비스가 이 프로그램과 다른 버전입니다"));
		break;
	case VPN_REMOTE_BUSY:
		AppendLog(_T("[gui] 다른 프로그램이 서비스에 붙어 있습니다"));
		break;
	default:
		AppendLog(_T("[gui] 서비스에 닿지 못합니다"));
		break;
	}

	/* 서비스에서 떨어지면 터널이 어떤 상태인지 이 프로그램은 모른다. 열려 있을 수도
	 * 있다. 모르는 것을 "연결됨"으로 보여 주지 않는다. */
	if (m_link != VPN_REMOTE_UP) {
		m_stopping = false;
		SetUiState(TUNNEL_STATE_STOPPED);
	}

	LoadDeviceId();
	RefreshPosture(false);
	UpdateStatus();

	return 0;
}

LRESULT CMainDlg::OnVpnDevice(WPARAM, LPARAM)
{
	LoadDeviceId();
	RefreshPosture(false);

	return 0;
}

/* 서비스가 명령을 받아 주지 않았다. */
LRESULT CMainDlg::OnVpnError(WPARAM wParam, LPARAM)
{
	switch (wParam) {
	case 1:		/* IPC_ERR_BUSY */
		AppendLog(_T("[gui] 서비스: 터널이 열려 있습니다. 먼저 해제하세요"));
		break;
	case 2:		/* IPC_ERR_NOT_ENROLLED */
		AppendLog(_T("[gui] 서비스: 등록되지 않은 기기입니다"));
		break;
	default:
		AppendLog(_T("[gui] 서비스가 요청을 받지 않았습니다"));
		break;
	}

	/* 연결을 눌러 "연결 중"으로 바꿔 둔 화면을 서비스의 실제 상태로 되돌린다. 열려
	 * 있었다면 곧 STATE 가 다시 온다. */
	if (!vpn_session_active(m_session)) {
		m_stopping = false;
		SetUiState(TUNNEL_STATE_STOPPED);
	}

	return 0;
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

	/* 서버가 정해 준 터널 주소. 수락과 keepalive 응답에 실려 온다. 관리자가 도중에
	 * 바꾸면 여기도 따라 바뀐다. 같은 값으로 다시 쓰지 않는다 — 10초마다 깜빡인다. */
	if (p.has_netcfg) {
		wchar_t ip[INET_ADDRSTRLEN];
		CString text;

		if (InetNtopW(AF_INET, &p.cfg_tun_ip, ip, INET_ADDRSTRLEN) != NULL) {
			if (p.cfg_routes != 0) {
				text.Format(_T("%s/%u  +경로 %u"), ip, (unsigned)p.cfg_prefix_len, (unsigned)p.cfg_routes);
			}
			else {
				text.Format(_T("%s/%u"), ip, (unsigned)p.cfg_prefix_len);
			}
			if (text != m_tun_text) {
				m_tun_text = text;
				SetDlgItemText(IDC_TUNIP, text);
			}
		}
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
		CString s = _T("거부 — ") + pkt::DenyReasonName(m_deny_reason);

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
		SetPostureCell(POSTURE_ROW_FIREWALL, 2, pkt::PostureStateName(m_reported.firewall));
		SetPostureCell(POSTURE_ROW_ANTIVIRUS, 2, pkt::PostureStateName(m_reported.antivirus));
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

/* ---- 진단 창 ----------------------------------------------------------- */
void CMainDlg::OnDiag()
{
	m_diag.Open(this);
}

/* 로그는 진단 창이 닫혀 있어도 여기에 쌓는다. 무엇이 잘못됐는지 보려고 진단 창을
 * 여는 것은 대개 일이 벌어진 뒤다. */
void CMainDlg::AppendLog(const CString &text)
{
	m_logLines.push_back(text);
	while (m_logLines.size() > LOG_MAX_LINES) {
		m_logLines.pop_front();
	}

	m_diag.LogAppended(text);
}

/* ---- 상태 -------------------------------------------------------------- */

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

	/* 터널이 닫히면 받았던 주소도 더는 내 것이 아니다. */
	if (stopped && !m_tun_text.IsEmpty()) {
		m_tun_text.Empty();
		SetDlgItemText(IDC_TUNIP, _T(""));
	}
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
		/* 서비스에 붙지 못했으면 연결을 눌러도 아무 일도 일어나지 않는다. 그 이유가
		 * 가장 먼저 보여야 한다. */
		if (m_link == VPN_REMOTE_UP) {
			sub = _T("서버 주소를 확인하고 연결을 누르세요");
		}
		else if (m_link == VPN_REMOTE_MISMATCH) {
			sub = _T("서비스가 이 프로그램과 다른 버전입니다 — vpn_service.exe install 로 다시 설치하세요");
			subColor = c.danger;
		}
		else if (m_link == VPN_REMOTE_BUSY) {
			sub = _T("다른 프로그램이 이미 서비스에 붙어 있습니다");
			subColor = c.danger;
		}
		else {
			sub = _T("서비스가 돌고 있지 않습니다 — 관리자 권한으로 vpn_service.exe install");
			subColor = c.danger;
		}
		dot = c.idle;
		break;
	}

	/* 거부됐으면 그 줄에 사유를 둔다. 탭을 옮기지 않아도 왜 막혔는지 보인다. 연결이
	 * 끊긴 뒤에도 남겨 둔다 — 첫 핸드셰이크에서 거부되면 곧바로 닫히므로, 지우면
	 * 사유를 볼 틈이 없다. 경과 시간으로도 덮지 않는다. */
	if (m_deny_reason != 0) {
		sub = _T("거부 사유: ") + pkt::DenyReasonName(m_deny_reason);
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

void CMainDlg::OnTimer(UINT_PTR id)
{
	if (id == TIMER_TICK_ID) {
		UpdateStatus();		/* 경과 시간 */

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

	/* 내용: 제목 + 포스처 표 */
	if (!m_rc_content.IsRectEmpty()) {
		ui::RoundRect(dc, m_rc_content, radius, c.card, c.border);
		ui::Text(dc, m_rc_content_title, _T("포스처"), m_fonts.strong, c.text,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE);

		rc.SetRect(m_rc_content.left + 1, m_sep_y, m_rc_content.right - 1, m_sep_y + 1);
		brush = CreateSolidBrush(c.border);
		FillRect(dc, &rc, brush);
		DeleteObject(brush);
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
	CRect rc(0, 0, 520, 200);

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
 *  +--------------------------------------------------------------+
 *  | 포스처                               [기기 등록...]  [진단] |  내용
 *  |--------------------------------------------------------------|
 *  | 포스처 표                                                    |
 *  +--------------------------------------------------------------+
 */
void CMainDlg::LayoutChildren(int cx, int cy)
{
	/* OnSize 는 컨트롤이 만들어지기 전에도 올 수 있다. */
	if (!m_posture.GetSafeHwnd()) {
		return;
	}

	CRect unit(0, 0, 4, 8);
	MapDialogRect(&unit);
	const int ux = unit.Width();
	const int uy = unit.Height();

	/* 다이얼로그 단위 -> 픽셀. .rc 와 같은 숫자를 쓸 수 있어 읽기 쉽다. */
	auto DX = [ux](int du) { return du * ux / 4; };
	auto DY = [uy](int du) { return du * uy / 8; };

	const int m = DX(8);
	const int gap = DY(6);

	m_rc_header.SetRect(m, DY(7), cx - m, DY(7) + DY(48));
	m_rc_content.SetRect(m, m_rc_header.bottom + gap, cx - m, cy - DY(8));

	/* 여러 컨트롤을 한 번에 옮긴다. 하나씩 MoveWindow 하면 중간 상태가 그려져 깜빡인다. */
	HDWP dwp = BeginDeferWindowPos(12);
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

	/* 내용의 윗줄: 왼쪽에 제목(그린다), 오른쪽에 버튼 */
	const int rowTop = m_rc_content.top + DY(3);
	const int rowH = DY(18);
	const int toolH = DY(14);
	const int toolY = rowTop + (rowH - toolH) / 2;
	int x = m_rc_content.right - DX(8);

	m_rc_content_title.SetRect(m_rc_content.left + DX(8), rowTop, x, rowTop + rowH);
	m_sep_y = rowTop + rowH + DY(2);

	x -= DX(42);
	Place(IDC_DIAG, x, toolY, DX(42), toolH);
	x -= DX(4) + DX(64);
	Place(IDC_ENROLL, x, toolY, DX(64), toolH);

	/* 리스트는 판의 둥근 모서리를 덮지 않게 조금 들여 넣는다. */
	const int listX = m_rc_content.left + DX(2);
	const int listY = m_sep_y + 1;
	const int listW = m_rc_content.Width() - 2 * DX(2);
	const int listH = m_rc_content.bottom - DY(4) - listY;

	Place(IDC_POSTURE, listX, listY, listW, listH);

	EndDeferWindowPos(dwp);

	/* 판의 자리가 바뀌었으니 바탕을 다시 그린다. */
	Invalidate(FALSE);
}

/* ---- 종료 -------------------------------------------------------------- */
void CMainDlg::OnDestroy()
{
	KillTimer(TIMER_TICK_ID);
	SaveSettings();

	/* 진단 창은 이 창이 가진 패킷과 로그를 가리킨다. 그것들보다 먼저 사라져야 한다. */
	if (m_diag.GetSafeHwnd()) {
		m_diag.DestroyWindow();
	}

	/* 이 창을 닫아도 터널은 닫히지 않는다. 터널은 서비스가 맡고 있고, destroy 는 그
	 * 서비스에서 떨어질 뿐이다. 다시 켜면 열려 있는 터널에 그대로 붙는다.
	 *
	 * 순서가 중요하다.
	 *  1) destroy 가 서비스에서 떨어지고 읽는 스레드가 끝날 때까지 기다린다. 이 시점
	 *     이후로는 새 PostMessage 가 생기지 않는다.
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
