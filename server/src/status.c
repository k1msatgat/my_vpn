#define _GNU_SOURCE	/* accept4 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "common.h"
#include "peer.h"
#include "acl.h"
#include "device.h"
#include "status.h"

server_stats_t stats;

static time_t started_at;

/* 세션 하나가 400바이트 남짓이다. MAX_PEERS 가 커지면 같이 키워야 하는데, 모자라면
 * 잘린 JSON 을 보내지 않고 아무것도 보내지 않는다. */
static char out[32768];
static size_t outlen;
static int overflow;

static void put(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (overflow) {
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(out + outlen, sizeof(out) - outlen, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= sizeof(out) - outlen) {
		overflow = 1;
		return;
	}
	outlen += (size_t)n;
}

static const char *state_str(uint8_t v)
{
	switch (v) {
	case POSTURE_STATE_ON:	return "on";
	case POSTURE_STATE_OFF:	return "off";
	default:		return "unknown";
	}
}

/* 문자열로 나가는 값은 전부 여기서 만든 것이다 (16진수, 점 찍은 주소, 고정된 단어).
 * 밖에서 들어온 글자가 그대로 실리지 않으므로 JSON 이스케이프가 필요 없다. */
static void build(int acl_enabled)
{
	char ids[DEVICE_ID_STRLEN];
	char ib[INET_ADDRSTRLEN], ob[INET_ADDRSTRLEN];
	time_t now = mono_now();
	const peer_t *p;
	int i, g, first = 1;

	outlen = 0;
	overflow = 0;

	put("{\"uptime_sec\":%ld,\"devices\":%d,", (long)(now - started_at), device_count());
	put("\"acl\":{\"enabled\":%s,\"rules\":%d},", acl_enabled ? "true" : "false", acl_count());

	put("\"counters\":{\"handshake_ok\":%llu,", (unsigned long long)stats.handshake_ok);
	put("\"deny_unknown_device\":%llu,", (unsigned long long)stats.deny[DENY_UNKNOWN_DEVICE]);
	put("\"deny_bad_auth\":%llu,", (unsigned long long)stats.deny[DENY_BAD_AUTH]);
	put("\"deny_tun_ip\":%llu,", (unsigned long long)stats.deny[DENY_TUN_IP]);
	put("\"deny_posture\":%llu,", (unsigned long long)stats.deny[DENY_POSTURE]);
	put("\"keepalive_dropped\":%llu,", (unsigned long long)stats.keepalive_dropped);
	put("\"bad_tag\":%llu,", (unsigned long long)stats.bad_tag);
	put("\"replayed\":%llu,", (unsigned long long)stats.replayed);
	put("\"unknown_session\":%llu,", (unsigned long long)stats.unknown_session);
	put("\"spoofed_src\":%llu,", (unsigned long long)stats.spoofed_src);
	put("\"acl_denied\":%llu,", (unsigned long long)stats.acl_denied);
	put("\"acl_denied_posture\":%llu,", (unsigned long long)stats.acl_denied_posture);
	put("\"acl_denied_user\":%llu,", (unsigned long long)stats.acl_denied_user);
	put("\"login_ok\":%llu,", (unsigned long long)stats.login_ok);
	put("\"login_failed\":%llu},", (unsigned long long)stats.login_failed);

	put("\"sessions\":[");
	for (i = 0; i < MAX_PEERS; i++) {
		p = peer_at(i);
		/* 등록용 세션은 기기의 세션이 아니다. 목록에 섞이면 없는 기기가 붙어 있는 것처럼 보인다. */
		if (p == NULL || p->enroll) {
			continue;
		}

		put("%s{\"tunnel_ip\":\"%s\",\"device_id\":\"%s\",", first ? "" : ",",
				ip_str(p->inner, ib), device_id_str(p->device_id, ids));
		put("\"endpoint\":\"%s:%d\",\"session_idx\":\"%08x\",",
				ip_str(p->outer.sin_addr, ob), ntohs(p->outer.sin_port), p->session_idx);
		put("\"age_sec\":%ld,\"idle_sec\":%ld,",
				(long)(now - p->established_at), (long)(now - p->last_seen));
		put("\"posture\":{\"os_build\":%u,\"firewall\":\"%s\",\"antivirus\":\"%s\"},",
				p->posture.os_build, state_str(p->posture.firewall),
				state_str(p->posture.antivirus));
		put("\"rx_packets\":%llu,\"rx_bytes\":%llu,",
				(unsigned long long)p->rx_packets, (unsigned long long)p->rx_bytes);
		put("\"tx_packets\":%llu,\"tx_bytes\":%llu,",
				(unsigned long long)p->tx_packets, (unsigned long long)p->tx_bytes);
		put("\"acl_denied\":%llu,", (unsigned long long)p->acl_denied);
		/* 이름과 그룹은 policy.c 가 정한 글자만 들어 있어 따옴표 없이 그대로 적어도 된다. */
		if (p->logged_in) {
			put("\"user\":{\"name\":\"%s\",\"expires\":%lld,\"groups\":[", p->user.name,
					(long long)p->user.expires);
			for (g = 0; g < p->user.n_groups; g++) {
				put("%s\"%s\"", g == 0 ? "" : ",", p->user.groups[g]);
			}
			put("]}}");
		}
		else {
			put("\"user\":null}");
		}
		first = 0;
	}
	put("]}\n");
}

int status_listen(const char *path)
{
	struct sockaddr_un addr;
	mode_t old;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "status: socket path too long\n");
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("status socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);

	/* 지난 실행이 남긴 소켓 파일이 있으면 bind 가 실패한다. */
	unlink(path);

	/* 소켓 파일은 bind 하는 순간 생긴다. 만든 뒤에 chmod 하면 그 사이에 열려 있는
	 * 틈이 생기므로, 처음부터 600 으로 만들어지게 umask 를 좁혀 둔다. */
	old = umask(0177);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("status bind");
		umask(old);
		close(fd);
		return -1;
	}
	umask(old);

	if (listen(fd, 8) < 0) {
		perror("status listen");
		close(fd);
		unlink(path);
		return -1;
	}

	started_at = mono_now();

	return fd;
}

void status_serve(int lfd, int acl_enabled)
{
	int fd;

	fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0) {
		return;
	}

	build(acl_enabled);

	/* 한 번에 다 못 쓰면 그만둔다. 터널을 나르는 루프라 여기서 기다릴 수 없다.
	 * 방금 만든 소켓의 버퍼는 이 크기보다 훨씬 크므로 평소에는 일어나지 않는다.
	 * 상대가 먼저 닫았을 때 SIGPIPE 로 서버가 죽지 않게 MSG_NOSIGNAL. */
	if (!overflow) {
		if (send(fd, out, outlen, MSG_NOSIGNAL) != (ssize_t)outlen) {
			fprintf(stderr, "status: short write\n");
		}
	}

	close(fd);
}
