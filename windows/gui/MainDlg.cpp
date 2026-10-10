#include "framework.h"
#include "resource.h"
#include "MainDlg.h"

#define TUN_PREFIX_LEN 24

CMainDlg::CMainDlg(CWnd *parent)
	: CDialogEx(IDD_MAIN, parent)
	, m_session(nullptr)
{
}

void CMainDlg::DoDataExchange(CDataExchange *pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LOG, m_log);
}

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
	ON_BN_CLICKED(IDC_CONNECT, &CMainDlg::OnConnect)
	ON_BN_CLICKED(IDC_DISCONNECT, &CMainDlg::OnDisconnect)
	ON_WM_DESTROY()
	ON_MESSAGE(WM_VPN_LOG, &CMainDlg::OnVpnLog)
	ON_MESSAGE(WM_VPN_STATE, &CMainDlg::OnVpnState)
	ON_MESSAGE(WM_VPN_DONE, &CMainDlg::OnVpnDone)
END_MESSAGE_MAP()

BOOL CMainDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetDlgItemText(IDC_ADAPTER, _T("my_vpn"));
	SetDlgItemText(IDC_SERVER, _T("49.247.139.39"));
	SetDlgItemText(IDC_PORT, _T("9000"));
	SetDlgItemText(IDC_TUNIP, _T("10.0.0.3"));
	CheckDlgButton(IDC_LOGPKT, BST_UNCHECKED);

	/* 창이 만들어진 뒤여야 PostMessage 대상 HWND 를 넘길 수 있다. */
	m_session = vpn_session_create(GetSafeHwnd());
	if (!m_session) {
		AfxMessageBox(_T("세션을 만들 수 없습니다."), MB_ICONERROR);
		EndDialog(IDCANCEL);
		return TRUE;
	}

	SetUiState(TUNNEL_STATE_STOPPED);

	return TRUE;
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
	cfg.log_packets = (IsDlgButtonChecked(IDC_LOGPKT) == BST_CHECKED) ? 1 : 0;

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
	SetUiState(static_cast<tunnel_state_t>(wParam));

	return 0;
}

LRESULT CMainDlg::OnVpnDone(WPARAM wParam, LPARAM)
{
	const int ret = static_cast<int>(wParam);

	/* 워커는 이미 반환했으므로 join 은 바로 돌아온다. 핸들을 정리해야 다시 연결할 수 있다. */
	vpn_session_join(m_session, VPN_STOP_TIMEOUT_MS);

	AppendLog(ret == 0 ? _T("[gui] 정상 종료") : _T("[gui] 오류로 종료"));
	SetUiState(TUNNEL_STATE_STOPPED);

	return 0;
}

/* ---- 종료 -------------------------------------------------------------- */
void CMainDlg::OnDestroy()
{
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

/* ---- UI 갱신 ----------------------------------------------------------- */
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
		break;
	case TUNNEL_STATE_CONNECTED:
		text = _T("연결됨");
		break;
	case TUNNEL_STATE_RECONNECTING:
		text = _T("재연결 중 (서버 무응답)");
		break;
	case TUNNEL_STATE_STOPPED:
	default:
		text = _T("중지됨");
		break;
	}

	SetDlgItemText(IDC_STATE, text);

	stopped = (state == TUNNEL_STATE_STOPPED) ? TRUE : FALSE;

	GetDlgItem(IDC_CONNECT)->EnableWindow(stopped);
	GetDlgItem(IDC_DISCONNECT)->EnableWindow(!stopped);

	/* 연결 중에 설정을 바꿔도 반영되지 않으므로 잠근다. */
	GetDlgItem(IDC_ADAPTER)->EnableWindow(stopped);
	GetDlgItem(IDC_SERVER)->EnableWindow(stopped);
	GetDlgItem(IDC_PORT)->EnableWindow(stopped);
	GetDlgItem(IDC_TUNIP)->EnableWindow(stopped);
	GetDlgItem(IDC_LOGPKT)->EnableWindow(stopped);
}
