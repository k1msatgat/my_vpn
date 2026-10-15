#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "posture.h"

#define KVER_PART_MAX	255	/* LINUX_VERSION_CODE 는 칸마다 8비트 */

/* 셸을 거치지 않고 절대 경로로 부른다. root 로 도는 프로그램이 PATH 를 따라가면 PATH 앞에
 * 놓인 가짜 nft 를 root 로 실행하게 된다. */
static const char *const NFT_PATHS[] = { "/usr/sbin/nft", "/sbin/nft" };

static uint32_t kernel_version(void)
{
	struct utsname u;
	unsigned int major = 0, minor = 0, patch = 0;

	if (uname(&u) < 0 || sscanf(u.release, "%u.%u.%u", &major, &minor, &patch) < 2) {
		return 0;
	}

	if (major > KVER_PART_MAX || minor > KVER_PART_MAX) {
		return 0;
	}
	if (patch > KVER_PART_MAX) {
		patch = KVER_PART_MAX;
	}

	return (uint32_t)(major << 16 | minor << 8 | patch);
}

const char *nft_path(void)
{
	size_t i;

	for (i = 0; i < sizeof(NFT_PATHS) / sizeof(NFT_PATHS[0]); i++) {
		if (access(NFT_PATHS[i], X_OK) == 0) {
			return NFT_PATHS[i];
		}
	}

	return NULL;
}

/* `nft list ruleset` 을 한 줄씩 읽어 posture.h 의 기준으로 판정한다. 규칙셋 문법을
 * 다 해석하지 않는다 — 체인 선언 줄은 "type filter hook input priority ...; policy drop;"
 * 처럼 한 줄에 나온다. */
static uint8_t collect_firewall(void)
{
	const char *nft = nft_path();
	char line[512];
	FILE *in;
	pid_t pid;
	int pipefd[2];
	int devnull;
	int status;
	int found = 0;

	if (nft == NULL || pipe(pipefd) < 0) {
		return POSTURE_STATE_UNKNOWN;
	}

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return POSTURE_STATE_UNKNOWN;
	}
	if (pid == 0) {
		/* 2초마다 부르므로 nft 의 오류 메시지는 버린다. 실패는 종료 코드로 안다. */
		devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execl(nft, "nft", "list", "ruleset", (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);

	in = fdopen(pipefd[0], "r");
	if (in == NULL) {
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return POSTURE_STATE_UNKNOWN;
	}

	/* 찾았어도 끝까지 읽는다. 중간에 닫으면 nft 가 SIGPIPE 로 끝나 성공 여부를 알 수 없다. */
	while (fgets(line, sizeof(line), in) != NULL) {
		if (strncmp(line, "table inet firewalld", strlen("table inet firewalld")) == 0 ||
				(strstr(line, "hook input") != NULL && strstr(line, "policy drop") != NULL)) {
			found = 1;
		}
	}
	fclose(in);

	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return POSTURE_STATE_UNKNOWN;
	}

	return found ? POSTURE_STATE_ON : POSTURE_STATE_OFF;
}

void posture_collect(posture_report_t *out)
{
	memset(out, 0, sizeof(*out));
	out->os_build = kernel_version();
	out->firewall = collect_firewall();
	out->antivirus = POSTURE_STATE_UNKNOWN;
}
