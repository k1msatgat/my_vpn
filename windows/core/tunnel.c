#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wintun.h"
#include "proto.h"
#include "tunnel.h"

#define HS_TIMEOUT_MSEC 1000
#define HS_RETRY 5

#define LOOP_TICK_MSEC 1000
#define SERVER_TIMEOUT_MSEC (SERVER_TIMEOUT_SEC * 1000)

#define TUN_RING_CAPACITY 0x400000 /* 4MiB */

/* 구버전 SDK / MinGW의 mstcpip.h에는 없다. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define RECV_EMPTY (-1)
#define RECV_ERROR (-2)

/* 어댑터 GUID를 고정해 두면 Windows가 매번 새 네트워크로 인식하지 않는다. */
static const GUID ADAPTER_GUID =
	{ 0x6f1d2c4a, 0x8b3e, 0x4d57, { 0x9a, 0x21, 0x5c, 0x7e, 0x0b, 0x4f, 0xd3, 0x18 } };

typedef struct session {
	uint32_t idx;
	uint64_t tx_counter;
	struct in_addr tun_ip;
	struct sockaddr_in server;
	uint64_t last_rx_ms;
	uint64_t last_tx_ms;
	uint64_t last_ka_ms;	/* 마지막 keepalive 송신. 생존 확인 경로를 10초에 한 번으로 묶는다 */
} session_t;

typedef struct tunnel {
	const tunnel_config_t *cfg;
	HANDLE stop_event;
	HANDLE sock_event;
	HANDLE tun_event;
	SOCKET sock;
	HMODULE wintun;
	WINTUN_ADAPTER_HANDLE adapter;
	WINTUN_SESSION_HANDLE tun;
	uint32_t bcast;		/* 터널 대역의 브로드캐스트 주소 (network byte order) */
	session_t session;
} tunnel_t;

/* ---- wintun.dll ---------------------------------------------------------- */
/* Wintun은 import lib 없이 DLL만 배포되므로 실행 중에 함수 주소를 얻는다. */
static WINTUN_CREATE_ADAPTER_FUNC *WintunCreateAdapter;
static WINTUN_CLOSE_ADAPTER_FUNC *WintunCloseAdapter;
static WINTUN_GET_ADAPTER_LUID_FUNC *WintunGetAdapterLUID;
static WINTUN_START_SESSION_FUNC *WintunStartSession;
static WINTUN_END_SESSION_FUNC *WintunEndSession;
static WINTUN_GET_READ_WAIT_EVENT_FUNC *WintunGetReadWaitEvent;
static WINTUN_RECEIVE_PACKET_FUNC *WintunReceivePacket;
static WINTUN_RELEASE_RECEIVE_PACKET_FUNC *WintunReleaseReceivePacket;
static WINTUN_ALLOCATE_SEND_PACKET_FUNC *WintunAllocateSendPacket;
static WINTUN_SEND_PACKET_FUNC *WintunSendPacket;

static HMODULE wintun_load(void)
{
	HMODULE mod;

	mod = LoadLibraryExW(L"wintun.dll", NULL,
			LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!mod) {
		return NULL;
	}

#define X(name) ((*(FARPROC *)&name = GetProcAddress(mod, #name)) == NULL)
	if (X(WintunCreateAdapter) || X(WintunCloseAdapter) || X(WintunGetAdapterLUID) ||
			X(WintunStartSession) || X(WintunEndSession) || X(WintunGetReadWaitEvent) ||
			X(WintunReceivePacket) || X(WintunReleaseReceivePacket) ||
			X(WintunAllocateSendPacket) || X(WintunSendPacket)) {
		DWORD err = GetLastError();

		FreeLibrary(mod);
		SetLastError(err);
		return NULL;
	}
#undef X

	return mod;
}

/* ---- callbacks ----------------------------------------------------------- */
static void tlog(const tunnel_t *t, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	if (!t->cfg->on_log) {
		return;
	}

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	t->cfg->on_log(t->cfg->ctx, line);
}

static void set_state(const tunnel_t *t, tunnel_state_t state)
{
	if (t->cfg->on_state) {
		t->cfg->on_state(t->cfg->ctx, state);
	}
}

/* ---- adapter / socket setup ---------------------------------------------- */
/* Linux의 `ip addr add` / `ip link set mtu` 에 해당한다. */
static int adapter_configure(tunnel_t *t)
{
	MIB_UNICASTIPADDRESS_ROW addr;
	MIB_IPINTERFACE_ROW iface;
	NET_LUID luid;
	DWORD err;

	WintunGetAdapterLUID(t->adapter, &luid);

	InitializeUnicastIpAddressEntry(&addr);
	addr.InterfaceLuid = luid;
	addr.Address.Ipv4.sin_family = AF_INET;
	addr.Address.Ipv4.sin_addr = t->cfg->tun_ip;
	addr.OnLinkPrefixLength = t->cfg->prefix_len;
	addr.DadState = IpDadStatePreferred;

	err = CreateUnicastIpAddressEntry(&addr);
	if (err != NO_ERROR && err != ERROR_OBJECT_ALREADY_EXISTS) {
		tlog(t, "CreateUnicastIpAddressEntry failed: %lu", err);
		return -1;
	}

	InitializeIpInterfaceEntry(&iface);
	iface.Family = AF_INET;
	iface.InterfaceLuid = luid;

	err = GetIpInterfaceEntry(&iface);
	if (err != NO_ERROR) {
		tlog(t, "GetIpInterfaceEntry failed: %lu", err);
		return -1;
	}

	iface.NlMtu = TUN_MTU;
	iface.SitePrefixLength = 0; /* IPv4에서는 0이어야 SetIpInterfaceEntry가 받아준다 */

	err = SetIpInterfaceEntry(&iface);
	if (err != NO_ERROR) {
		tlog(t, "SetIpInterfaceEntry failed: %lu", err);
		return -1;
	}

	return 0;
}

static int socket_open(tunnel_t *t)
{
	struct sockaddr_in local;
	BOOL connreset = FALSE;
	DWORD bytes = 0;

	t->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (t->sock == INVALID_SOCKET) {
		tlog(t, "socket failed: %d", WSAGetLastError());
		return -1;
	}

	/* 보내기 전에 recvfrom을 불러도 WSAEINVAL이 나지 않도록 미리 bind 해 둔다. */
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;
	if (bind(t->sock, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) {
		tlog(t, "bind failed: %d", WSAGetLastError());
		return -1;
	}

	/* Windows는 ICMP port unreachable을 받으면 다음 recvfrom을 WSAECONNRESET으로 실패시킨다.
	 * 서버가 잠시 내려가 있는 동안에도 계속 돌아야 하므로 이 동작을 끈다. */
	if (WSAIoctl(t->sock, SIO_UDP_CONNRESET, &connreset, sizeof(connreset),
				NULL, 0, &bytes, NULL, NULL) == SOCKET_ERROR) {
		tlog(t, "SIO_UDP_CONNRESET failed: %d", WSAGetLastError());
	}

	/* auto-reset 이벤트. WSAEventSelect를 걸면 소켓은 non-blocking이 된다. */
	t->sock_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!t->sock_event) {
		tlog(t, "CreateEvent failed: %lu", GetLastError());
		return -1;
	}

	if (WSAEventSelect(t->sock, t->sock_event, FD_READ) == SOCKET_ERROR) {
		tlog(t, "WSAEventSelect failed: %d", WSAGetLastError());
		return -1;
	}

	return 0;
}

/* ---- udp helpers --------------------------------------------------------- */
/* 받은 바이트 수를 돌려준다. 큐가 비었으면 RECV_EMPTY, 복구할 수 없는 오류는 RECV_ERROR. */
static int udp_recv(tunnel_t *t, uint8_t *buf, int cap, struct sockaddr_in *from)
{
	int fromlen;
	int n;
	int err;

	for (;;) {
		fromlen = (int)sizeof(*from);
		n = recvfrom(t->sock, (char *)buf, cap, 0, (struct sockaddr *)from, &fromlen);
		if (n != SOCKET_ERROR) {
			return n;
		}

		err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK) {
			return RECV_EMPTY;
		}
		if (err == WSAECONNRESET || err == WSAEMSGSIZE) {
			continue;
		}

		tlog(t, "recvfrom failed: %d", err);
		return RECV_ERROR;
	}
}

static void udp_send(tunnel_t *t, const uint8_t *buf, int len)
{
	session_t *session = &t->session;

	if (sendto(t->sock, (const char *)buf, len, 0,
				(struct sockaddr *)&session->server, sizeof(session->server)) == SOCKET_ERROR) {
		tlog(t, "sendto failed: %d", WSAGetLastError());
	}
	session->last_tx_ms = GetTickCount64();
}

static int from_server(const session_t *session, const struct sockaddr_in *from)
{
	return from->sin_addr.s_addr == session->server.sin_addr.s_addr &&
		from->sin_port == session->server.sin_port;
}

/* ---- outbound filter ----------------------------------------------------- */
/* 터널 대역의 브로드캐스트 주소. prefix가 /32면 브로드캐스트가 없으므로 0을 돌려준다
 * (dst 0.0.0.0은 어차피 전달 대상이 아니라 비교에 써도 무해하다). */
static uint32_t subnet_broadcast(struct in_addr addr, uint8_t prefix_len)
{
	uint32_t mask;

	if (prefix_len == 0) {
		return 0xFFFFFFFFu;
	}
	if (prefix_len >= 32) {
		return 0;
	}

	mask = htonl(0xFFFFFFFFu << (32 - prefix_len));

	return (addr.s_addr & mask) | ~mask;
}

/* 서버는 inner dst로 피어를 1:1로 찾아(peer_lookup) 그 피어에게만 보내므로,
 * 멀티캐스트나 브로드캐스트는 애초에 전달할 상대가 없다.
 *
 * Linux의 tun0은 조용하지만 Windows는 어댑터가 올라오는 순간부터 모든 인터페이스에
 * mDNS / LLMNR / SSDP / IGMP를 꾸준히 내보낸다. 이를 그대로 터널에 넣으면
 *   1) 서버는 자기 tun에 write한 뒤 커널이 버리므로 응답이 돌아오지 않고,
 *   2) udp_send()가 last_tx_ms를 10초 안에 계속 갱신해 유휴 keepalive가 영구히 억제되어
 * 결국 서버가 살아 있는데도 30초마다 재핸드셰이크가 돈다. */
static int deliverable_dst(const tunnel_t *t, const uint8_t *ip)
{
	uint32_t dst;

	/* 224.0.0.0/4 멀티캐스트와 240.0.0.0/4 (255.255.255.255 포함) */
	if (ip[IP_DST_OFF] >= 224) {
		return 0;
	}

	memcpy(&dst, ip + IP_DST_OFF, IP_ADDR_LEN);

	return dst != t->bcast;
}

/* ---- protocol ------------------------------------------------------------ */
/* 0: 세션 수립, -1: 응답 없음 / 오류 / 중지 요청 */
static int do_handshake(tunnel_t *t)
{
	session_t *session = &t->session;
	HANDLE waits[2] = { t->stop_event, t->sock_event };
	uint8_t req[sizeof(msg_header_t) + sizeof(struct in_addr)];
	uint8_t res[BUF_SIZE];
	msg_header_t msg_header;
	struct sockaddr_in from;
	uint64_t deadline;
	uint64_t now;
	DWORD w;
	int off;
	int n;
	int tries;

	memset(&msg_header, 0, sizeof(msg_header_t));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_REQ_HANDSHAKE;
	msg_header.session_idx = 0;

	off = msg_encode(req, sizeof(req), &msg_header);

	memcpy(req + off, &session->tun_ip, sizeof(session->tun_ip));
	off += (int)sizeof(session->tun_ip);

	for (tries = 1; tries <= HS_RETRY; tries++) {
		tlog(t, "[HS] request %d/%d", tries, HS_RETRY);
		udp_send(t, req, off);

		deadline = GetTickCount64() + HS_TIMEOUT_MSEC;
		for (;;) {
			while ((n = udp_recv(t, res, (int)sizeof(res), &from)) >= 0) {
				if (!from_server(session, &from)) {
					continue;
				}

				if (msg_decode(res, (size_t)n, &msg_header) < 0) {
					continue;
				}

				if (msg_header.version != PROTOCOL_VERSION || msg_header.type != MSG_TYPE_RES_HANDSHAKE ||
						msg_header.session_idx == 0) {
					continue;
				}

				if (session->idx != msg_header.session_idx) {
					session->idx = msg_header.session_idx;
					session->tx_counter = 0;
				}

				tlog(t, "[HS] established idx=%08x", session->idx);
				return 0;
			}
			if (n == RECV_ERROR) {
				return -1;
			}

			now = GetTickCount64();
			if (now >= deadline) {
				break;
			}

			w = WaitForMultipleObjects(2, waits, FALSE, (DWORD)(deadline - now));
			if (w == WAIT_OBJECT_0) {
				return -1;
			}
			if (w == WAIT_FAILED) {
				tlog(t, "WaitForMultipleObjects failed: %lu", GetLastError());
				return -1;
			}
		}
	}

	tlog(t, "[HS] no response from server");
	return -1;
}

static void send_keepalive(tunnel_t *t)
{
	msg_header_t msg_header;
	uint8_t packet[sizeof(msg_header_t)];
	int off;

	memset(&msg_header, 0, sizeof(msg_header_t));
	msg_header.version = PROTOCOL_VERSION;
	msg_header.type = MSG_TYPE_KEEPALIVE;
	msg_header.session_idx = t->session.idx;
	off = msg_encode(packet, sizeof(packet), &msg_header);

	udp_send(t, packet, off);
	t->session.last_ka_ms = t->session.last_tx_ms;

	if (t->cfg->log_packets) {
		tlog(t, "[keepalive] sent");
	}
}

/* TUN -> UDP. Wintun 링버퍼에 쌓인 패킷을 모두 비운다. */
static int pump_tun(tunnel_t *t)
{
	session_t *session = &t->session;
	uint8_t packet[BUF_SIZE];
	msg_header_t msg_header;
	data_header_t data_header;
	BYTE *ip;
	DWORD n;
	DWORD err;
	int off;

	for (;;) {
		ip = WintunReceivePacket(t->tun, &n);
		if (!ip) {
			err = GetLastError();
			if (err == ERROR_NO_MORE_ITEMS) {
				return 0;
			}
			tlog(t, "WintunReceivePacket failed: %lu", err);
			return -1;
		}

		if (n < IP_MIN_HDR || IP_VERSION(ip) != 4 || n > sizeof(packet) - PKT_HDR_LEN) {
			if (t->cfg->log_packets) {
				tlog(t, "[skip] non-IPv4 (ver=%d, %lu bytes)", IP_VERSION(ip), n);
			}
		}
		else if (!deliverable_dst(t, ip)) {
			if (t->cfg->log_packets) {
				tlog(t, "[skip] non-unicast dst (%lu bytes, proto=%d)", n, ip[IP_PROTO_OFF]);
			}
		}
		else {
			if (t->cfg->log_packets) {
				tlog(t, "[tun->udp] %lu bytes, proto=%d", n, ip[IP_PROTO_OFF]);
			}

			memset(&msg_header, 0, sizeof(msg_header_t));
			msg_header.version = PROTOCOL_VERSION;
			msg_header.type = MSG_TYPE_DATA;
			msg_header.session_idx = session->idx;
			data_header.counter = session->tx_counter++;

			off = msg_encode(packet, sizeof(packet), &msg_header);
			off += data_encode(packet + off, sizeof(packet) - off, &data_header);

			/* 패킷이 Wintun 링버퍼 안에 있어 Linux처럼 헤드룸을 둘 수 없으므로 한 번 복사한다. */
			memcpy(packet + off, ip, n);

			udp_send(t, packet, off + (int)n);
		}

		WintunReleaseReceivePacket(t->tun, ip);
	}
}

/* UDP -> TUN. 소켓 수신 큐를 모두 비운다. */
static int pump_udp(tunnel_t *t)
{
	session_t *session = &t->session;
	uint8_t packet[BUF_SIZE];
	msg_header_t msg_header;
	data_header_t data_header;
	struct sockaddr_in from;
	uint8_t *ip;
	BYTE *out;
	int off;
	int n;

	while ((n = udp_recv(t, packet, (int)sizeof(packet), &from)) >= 0) {
		if (!from_server(session, &from)) {
			tlog(t, "[drop] not from server");
			continue;
		}

		off = msg_decode(packet, (size_t)n, &msg_header);
		if (off < 0 || msg_header.version != PROTOCOL_VERSION) {
			continue;
		}

		if (msg_header.session_idx != session->idx) {
			tlog(t, "[drop] wrong session [%08x][%08x]", msg_header.session_idx, session->idx);
			continue;
		}

		session->last_rx_ms = GetTickCount64();

		switch (msg_header.type) {
			case MSG_TYPE_KEEPALIVE:
				/* last_rx_ms는 위에서 이미 갱신했다. 세션이 살아 있다는 증거라
				 * 유휴 시나리오에서 확인할 수 있게 로그를 남긴다. */
				if (t->cfg->log_packets) {
					tlog(t, "[keepalive] recv");
				}
				continue;
			case MSG_TYPE_DATA:
				break;
			default:
				tlog(t, "[drop] unknown type(%d bytes)", n);
				continue;
		}

		if (data_decode(packet + off, (size_t)(n - off), &data_header) < 0) {
			tlog(t, "[drop] short data header");
			continue;
		}

		off += (int)sizeof(data_header_t);

		n -= off;

		ip = packet + off;
		if (n < IP_MIN_HDR || IP_VERSION(ip) != 4) {
			tlog(t, "[drop] udp: not IPv4 (%d bytes)", n);
			continue;
		}

		if (t->cfg->log_packets) {
			tlog(t, "[udp->tun] %d bytes, proto=%d", n, ip[IP_PROTO_OFF]);
		}

		out = WintunAllocateSendPacket(t->tun, (DWORD)n);
		if (!out) {
			tlog(t, "WintunAllocateSendPacket failed: %lu", GetLastError());
			continue;
		}

		memcpy(out, ip, (size_t)n);
		WintunSendPacket(t->tun, out);
	}

	return n == RECV_ERROR ? -1 : 0;
}

/* ---- entry --------------------------------------------------------------- */
int tunnel_run(const tunnel_config_t *cfg, HANDLE stop_event)
{
	tunnel_t tunnel;
	tunnel_t *t = &tunnel;
	session_t *session = &t->session;
	HANDLE waits[3];
	WSADATA wsa;
	char server_str[INET_ADDRSTRLEN];
	uint64_t now;
	DWORD err;
	DWORD w;
	int wsa_ready = 0;
	int ret = 1;

	memset(t, 0, sizeof(*t));
	t->cfg = cfg;
	t->stop_event = stop_event;
	t->sock = INVALID_SOCKET;

	t->bcast = subnet_broadcast(cfg->tun_ip, cfg->prefix_len);

	session->tun_ip = cfg->tun_ip;
	session->server.sin_family = AF_INET;
	session->server.sin_addr = cfg->server_ip;
	session->server.sin_port = htons(cfg->server_port);
	inet_ntop(AF_INET, &cfg->server_ip, server_str, sizeof(server_str));

	set_state(t, TUNNEL_STATE_CONNECTING);

	err = (DWORD)WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0) {
		tlog(t, "WSAStartup failed: %lu", err);
		goto out;
	}
	wsa_ready = 1;

	t->wintun = wintun_load();
	if (!t->wintun) {
		tlog(t, "failed to load wintun.dll: %lu", GetLastError());
		goto out;
	}

	t->adapter = WintunCreateAdapter(cfg->adapter_name, L"my_vpn", &ADAPTER_GUID);
	if (!t->adapter) {
		err = GetLastError();
		tlog(t, "WintunCreateAdapter failed: %lu%s", err,
				err == ERROR_ACCESS_DENIED ? " (run as Administrator)" : "");
		goto out;
	}

	if (adapter_configure(t) < 0) {
		goto out;
	}

	t->tun = WintunStartSession(t->adapter, TUN_RING_CAPACITY);
	if (!t->tun) {
		tlog(t, "WintunStartSession failed: %lu", GetLastError());
		goto out;
	}
	t->tun_event = WintunGetReadWaitEvent(t->tun);

	tlog(t, "[%ls] tun device ready", cfg->adapter_name);

	if (socket_open(t) < 0) {
		goto out;
	}

	tlog(t, "[%ls] tunneling to %s:%u", cfg->adapter_name, server_str, cfg->server_port);

	if (do_handshake(t) < 0) {
		goto out;
	}

	session->last_rx_ms = GetTickCount64();
	set_state(t, TUNNEL_STATE_CONNECTED);

	waits[0] = t->stop_event;
	waits[1] = t->tun_event;
	waits[2] = t->sock_event;

	for (;;) {
		/* Linux의 epoll_wait(…, 1000)에 해당. 어느 쪽이 깨웠는지와 관계없이
		 * 아래에서 TUN과 소켓을 둘 다 비우므로 한쪽이 굶지 않는다. */
		w = WaitForMultipleObjects(3, waits, FALSE, LOOP_TICK_MSEC);
		if (w == WAIT_OBJECT_0) {
			break;
		}
		if (w == WAIT_FAILED) {
			tlog(t, "WaitForMultipleObjects failed: %lu", GetLastError());
			goto out;
		}

		now = GetTickCount64();
		if (now - session->last_rx_ms > SERVER_TIMEOUT_MSEC) {
			tlog(t, "[HS] no rx from server for %ds, re-handshake", SERVER_TIMEOUT_SEC);
			set_state(t, TUNNEL_STATE_RECONNECTING);
			if (do_handshake(t) < 0) {
				tlog(t, "[HS] failed re-handshake[%s][%u], retry in %ds",
						server_str, cfg->server_port, SERVER_TIMEOUT_SEC);
			}
			else {
				set_state(t, TUNNEL_STATE_CONNECTED);
			}
			now = GetTickCount64();
			session->last_rx_ms = now;
		}

		/* keepalive가 맡은 두 가지를 따로 본다.
		 *  - NAT 매핑 유지: 내가 10초 동안 아무것도 보내지 않았으면 보낸다.
		 *  - 생존 확인: 서버가 10초 동안 조용하면 응답을 유도한다. 데이터 패킷은 서버가
		 *    버릴 수 있어 응답을 보장하지 못하지만, keepalive는 서버가 반드시 되돌려준다.
		 *    이 경로가 없으면 "응답 없는 송신"이 이어질 때 keepalive가 억제된 채
		 *    재핸드셰이크까지 가 버린다. last_ka_ms로 10초에 한 번만 나가게 묶는다. */
		if (now - session->last_tx_ms > KEEPALIVE_INTERVAL_MSEC ||
				(now - session->last_rx_ms > KEEPALIVE_INTERVAL_MSEC &&
				 now - session->last_ka_ms > KEEPALIVE_INTERVAL_MSEC)) {
			send_keepalive(t);
		}

		if (pump_tun(t) < 0 || pump_udp(t) < 0) {
			goto out;
		}
	}
	ret = 0;

out:
	if (t->sock != INVALID_SOCKET) {
		closesocket(t->sock);
	}
	if (t->sock_event) {
		CloseHandle(t->sock_event);
	}
	if (t->tun) {
		WintunEndSession(t->tun);
	}
	if (t->adapter) {
		WintunCloseAdapter(t->adapter);
	}
	if (t->wintun) {
		FreeLibrary(t->wintun);
	}
	if (wsa_ready) {
		WSACleanup();
	}

	set_state(t, TUNNEL_STATE_STOPPED);
	return ret;
}
