#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "common.h"
#include "device.h"

static device_t devices[MAX_DEVICES];
static int ndevices;

void device_init(void)
{
	memset(devices, 0, sizeof(devices));
	ndevices = 0;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* 정확히 len 바이트 분량의 16진 문자열만 받는다. 짧거나 길면 실패. */
static int parse_hex(const char *s, uint8_t *out, size_t len)
{
	size_t i;
	int hi, lo;

	if (strlen(s) != len * 2) {
		return -1;
	}

	for (i = 0; i < len; i++) {
		hi = hex_val(s[i * 2]);
		lo = hex_val(s[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)(hi << 4 | lo);
	}

	return 0;
}

const char *device_id_str(const uint8_t id[DEVICE_ID_LEN], char *dst)
{
	int i;

	for (i = 0; i < DEVICE_ID_LEN; i++) {
		sprintf(dst + i * 2, "%02x", id[i]);
	}

	return dst;
}

int device_load(const char *path)
{
	FILE *fp;
	struct stat st;
	char line[256];
	char s_id[80], s_key[80], s_ip[32];
	int lineno = 0;
	int i;

	fp = fopen(path, "r");
	if (fp == NULL) {
		perror("fopen device file");
		return -1;
	}

	/* 이 파일에는 모든 기기의 키가 평문으로 들어 있다. 다른 계정이 읽을 수 있게
	 * 돼 있으면 읽지 않는다. ssh 가 권한이 열린 개인키를 거부하는 것과 같은 이유다.
	 * 경로가 아니라 이미 연 파일에 fstat 을 한다 — 검사한 파일과 읽는 파일이
	 * 같다는 것이 보장된다. */
	if (fstat(fileno(fp), &st) < 0) {
		perror("fstat device file");
		fclose(fp);
		return -1;
	}
	if (st.st_mode & (S_IRWXG | S_IRWXO)) {
		fprintf(stderr, "device: %s is accessible by group/others (mode %04o), run: chmod 600 %s\n",
				path, (unsigned)(st.st_mode & 07777), path);
		fclose(fp);
		return -1;
	}

	device_init();

	while (fgets(line, sizeof(line), fp) != NULL) {
		device_t *d;
		char *hash;

		lineno++;

		hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = '\0';
		}

		/* 빈 줄과 주석만 있는 줄 */
		if (sscanf(line, "%79s", s_id) != 1) {
			continue;
		}

		if (sscanf(line, "%79s %79s %31s", s_id, s_key, s_ip) != 3) {
			fprintf(stderr, "device: line %d: expected <id> <key> <tunnel-ip>\n", lineno);
			goto fail;
		}

		if (ndevices >= MAX_DEVICES) {
			fprintf(stderr, "device: too many devices (max %d)\n", MAX_DEVICES);
			goto fail;
		}

		d = &devices[ndevices];

		if (parse_hex(s_id, d->id, DEVICE_ID_LEN) < 0) {
			fprintf(stderr, "device: line %d: id must be %d hex digits\n", lineno, DEVICE_ID_LEN * 2);
			goto fail;
		}
		if (parse_hex(s_key, d->key, AUTH_KEY_LEN) < 0) {
			fprintf(stderr, "device: line %d: key must be %d hex digits\n", lineno, AUTH_KEY_LEN * 2);
			goto fail;
		}
		if (inet_pton(AF_INET, s_ip, &d->tun_ip) != 1) {
			fprintf(stderr, "device: line %d: bad tunnel ip '%s'\n", lineno, s_ip);
			goto fail;
		}

		/* 같은 ID 가 두 번 나오면 어느 키가 맞는지 정할 수 없고, 같은 터널 IP 를 두
		 * 기기에 주면 서로의 세션을 밀어낸다. 조용히 한쪽을 고르지 않고 거부한다. */
		for (i = 0; i < ndevices; i++) {
			if (memcmp(devices[i].id, d->id, DEVICE_ID_LEN) == 0) {
				fprintf(stderr, "device: line %d: duplicate id\n", lineno);
				goto fail;
			}
			if (devices[i].tun_ip.s_addr == d->tun_ip.s_addr) {
				fprintf(stderr, "device: line %d: tunnel ip %s already assigned\n", lineno, s_ip);
				goto fail;
			}
		}

		ndevices++;
	}

	fclose(fp);
	return ndevices;

fail:
	fclose(fp);
	device_init();
	return -1;
}

device_t *device_find(const uint8_t id[DEVICE_ID_LEN])
{
	int i;

	for (i = 0; i < ndevices; i++) {
		if (memcmp(devices[i].id, id, DEVICE_ID_LEN) == 0) {
			return &devices[i];
		}
	}

	return NULL;
}

/* 키는 찍지 않는다. 로그는 설정 파일보다 넓게 읽힌다. */
void device_dump(void)
{
	char ids[DEVICE_ID_STRLEN];
	char ib[INET_ADDRSTRLEN];
	int i;

	printf("--- devices (%d) ---\n", ndevices);
	for (i = 0; i < ndevices; i++) {
		printf("[%d] %s -> %s\n", i, device_id_str(devices[i].id, ids),
				ip_str(devices[i].tun_ip, ib));
	}
}
