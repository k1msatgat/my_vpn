#include <winsock2.h>
#include <windows.h>
#include <string.h>

#include "vpn_session.h"

struct vpn_session {
	HWND notify;
	HANDLE thread;		/* 워커가 돌거나 끝났지만 아직 join 안 된 상태면 non-NULL */
	HANDLE stop_event;	/* manual-reset. tunnel_run이 모든 대기 지점에서 같이 본다 */
	tunnel_config_t cfg;	/* 워커가 보는 사본 */

	/* 패킷 링버퍼. 워커가 넣고 UI 스레드가 꺼내므로 보호가 필요하다. */
	CRITICAL_SECTION lock;
	tunnel_packet_t *ring;
	int ring_head;
	int ring_count;
	uint64_t ring_lost;
	volatile LONG notify_pending;
};

/* ---- worker -> UI ------------------------------------------------------- */
/* 콜백은 워커 스레드에서 불린다. 여기서 컨트롤을 직접 건드리면 안 되고,
 * SendMessage도 쓸 수 없다. SendMessage는 UI 스레드가 처리할 때까지 블로킹되는데,
 * 사용자가 창을 닫는 중이면 UI 스레드는 이 워커가 끝나기를 기다리고 있어서
 * 서로를 기다리는 교착이 된다. PostMessage는 큐에 넣고 바로 돌아온다. */
static void on_log_cb(void *ctx, const char *line)
{
	vpn_session_t *s = (vpn_session_t *)ctx;
	size_t size;
	char *copy;

	/* line은 tunnel.c의 스택 버퍼라 돌아가면 사라진다. 복사해서 소유권을 넘긴다. */
	size = strlen(line) + 1;
	copy = (char *)HeapAlloc(GetProcessHeap(), 0, size);
	if (!copy) {
		return;
	}
	memcpy(copy, line, size);

	/* 창이 이미 사라졌으면 받을 쪽이 없으므로 여기서 해제한다. */
	if (!PostMessageW(s->notify, WM_VPN_LOG, 0, (LPARAM)copy)) {
		HeapFree(GetProcessHeap(), 0, copy);
	}
}

/* 상태는 스칼라라 할당이 없고, 따라서 새는 것도 없다. */
static void on_state_cb(void *ctx, tunnel_state_t state)
{
	vpn_session_t *s = (vpn_session_t *)ctx;

	PostMessageW(s->notify, WM_VPN_STATE, (WPARAM)state, 0);
}

/* 패킷은 초당 수천 건이 될 수 있어 건당 PostMessage 를 하면 메시지 큐가 넘친다.
 * 링버퍼에 쌓고 "처리 안 된 알림이 없을 때만" 한 번 깨운다. 그래서 패킷 수와
 * 무관하게 큐에 떠 있는 알림은 최대 하나다. */
static void on_packet_cb(void *ctx, const tunnel_packet_t *p)
{
	vpn_session_t *s = (vpn_session_t *)ctx;

	EnterCriticalSection(&s->lock);
	if (s->ring_count == VPN_PACKET_RING) {
		/* 꽉 찼으면 가장 오래된 것을 버린다. 최신 패킷을 살리는 쪽이 분석에 쓸모 있다. */
		s->ring_head = (s->ring_head + 1) % VPN_PACKET_RING;
		s->ring_count--;
		s->ring_lost++;
	}
	s->ring[(s->ring_head + s->ring_count) % VPN_PACKET_RING] = *p;
	s->ring_count++;
	LeaveCriticalSection(&s->lock);

	if (InterlockedExchange(&s->notify_pending, 1) == 0) {
		if (!PostMessageW(s->notify, WM_VPN_PACKETS, 0, 0)) {
			InterlockedExchange(&s->notify_pending, 0);
		}
	}
}

int vpn_session_drain_packets(vpn_session_t *s, tunnel_packet_t *out, int max)
{
	int n = 0;

	if (!s || !out || max <= 0) {
		return 0;
	}

	/* 비우기 **전에** 플래그를 내린다. 꺼내는 중에 들어온 패킷이 새 알림을
	 * 보낼 수 있어야 한다. 순서를 바꾸면 그 패킷의 깨우기가 억제되어
	 * 링에 남은 채 다음 패킷까지 묻힌다. */
	InterlockedExchange(&s->notify_pending, 0);

	EnterCriticalSection(&s->lock);
	while (n < max && s->ring_count > 0) {
		out[n++] = s->ring[s->ring_head];
		s->ring_head = (s->ring_head + 1) % VPN_PACKET_RING;
		s->ring_count--;
	}
	LeaveCriticalSection(&s->lock);

	return n;
}

uint64_t vpn_session_lost_packets(vpn_session_t *s)
{
	uint64_t v;

	if (!s) {
		return 0;
	}

	EnterCriticalSection(&s->lock);
	v = s->ring_lost;
	LeaveCriticalSection(&s->lock);

	return v;
}

static DWORD WINAPI worker(LPVOID arg)
{
	vpn_session_t *s = (vpn_session_t *)arg;
	int ret;

	ret = tunnel_run(&s->cfg, s->stop_event);

	/* tunnel_run이 마지막에 보내는 TUNNEL_STATE_STOPPED보다 이 메시지가 뒤에 오도록
	 * 반환 후에 보낸다. UI는 WM_VPN_DONE에서 join하면 된다. */
	PostMessageW(s->notify, WM_VPN_DONE, (WPARAM)ret, 0);

	return (DWORD)ret;
}

/* ---- lifecycle ---------------------------------------------------------- */
vpn_session_t *vpn_session_create(HWND notify)
{
	vpn_session_t *s;

	s = (vpn_session_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
	if (!s) {
		return NULL;
	}

	s->notify = notify;

	/* manual-reset: tunnel_run은 핸드셰이크 대기와 메인 루프 양쪽에서 이 이벤트를 보므로
	 * 한 번 signaled 되면 계속 signaled 여야 한다. */
	s->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!s->stop_event) {
		HeapFree(GetProcessHeap(), 0, s);
		return NULL;
	}

	s->ring = (tunnel_packet_t *)HeapAlloc(GetProcessHeap(), 0,
			sizeof(*s->ring) * VPN_PACKET_RING);
	if (!s->ring) {
		CloseHandle(s->stop_event);
		HeapFree(GetProcessHeap(), 0, s);
		return NULL;
	}

	InitializeCriticalSection(&s->lock);

	return s;
}

int vpn_session_start(vpn_session_t *s, const tunnel_config_t *cfg)
{
	if (!s || s->thread) {
		return -1;
	}

	ResetEvent(s->stop_event);

	/* tunnel_config_t는 포인터를 품지 않는 POD(adapter_name도 배열)이므로
	 * 구조체 복사만으로 워커가 쓸 사본이 완성된다. */
	s->cfg = *cfg;
	s->cfg.on_log = on_log_cb;
	s->cfg.on_state = on_state_cb;
	s->cfg.on_packet = on_packet_cb;
	s->cfg.ctx = s;

	/* 새 연결이면 지난 세션의 패킷은 버린다. */
	EnterCriticalSection(&s->lock);
	s->ring_head = 0;
	s->ring_count = 0;
	s->ring_lost = 0;
	LeaveCriticalSection(&s->lock);
	InterlockedExchange(&s->notify_pending, 0);

	s->thread = CreateThread(NULL, 0, worker, s, 0, NULL);
	if (!s->thread) {
		return -1;
	}

	return 0;
}

void vpn_session_stop(vpn_session_t *s)
{
	if (s && s->thread) {
		SetEvent(s->stop_event);
	}
}

int vpn_session_join(vpn_session_t *s, DWORD timeout_ms)
{
	if (!s || !s->thread) {
		return 0;
	}

	if (WaitForSingleObject(s->thread, timeout_ms) != WAIT_OBJECT_0) {
		return -1;
	}

	CloseHandle(s->thread);
	s->thread = NULL;

	return 0;
}

int vpn_session_active(const vpn_session_t *s)
{
	return s && s->thread;
}

void vpn_session_destroy(vpn_session_t *s)
{
	if (!s) {
		return;
	}

	if (s->thread) {
		SetEvent(s->stop_event);

		/* 타임아웃이 나도 TerminateThread는 쓰지 않는다. 강제로 끊으면 tunnel_run의
		 * 정리 구간이 돌지 못해 Wintun 어댑터와 터널 IP가 남는다.
		 * 이 경우 워커가 아직 s와 stop_event를 보고 있으므로 해제하지 않고 그대로 둔다
		 * (프로세스 종료가 임박한 시점이고, 쓰고 있는 메모리를 푸는 것보다 안전하다). */
		if (vpn_session_join(s, VPN_STOP_TIMEOUT_MS) < 0) {
			return;
		}
	}

	CloseHandle(s->stop_event);
	DeleteCriticalSection(&s->lock);
	HeapFree(GetProcessHeap(), 0, s->ring);
	HeapFree(GetProcessHeap(), 0, s);
}

/* ---- UI helpers --------------------------------------------------------- */
void vpn_session_drain(HWND notify)
{
	MSG msg;

	while (PeekMessageW(&msg, notify, WM_VPN_LOG, WM_VPN_LOG, PM_REMOVE)) {
		vpn_session_free_log((char *)msg.lParam);
	}
}

void vpn_session_free_log(char *line)
{
	if (line) {
		HeapFree(GetProcessHeap(), 0, line);
	}
}
