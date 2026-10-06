#include <stdio.h>
#include <string.h>
#include <endian.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include "common.h"

const char *ip_str(struct in_addr a, char *dist)
{
	return inet_ntop(AF_INET, &a, dist, INET_ADDRSTRLEN);
}

int tun_alloc(char *dev)
{
	struct ifreq ifr;
	int fd, err;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0){
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	if (*dev){
		strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
	}

	err = ioctl(fd, TUNSETIFF, (void*)&ifr);
	if (err < 0){
		close(fd);
		return err;
	}

	strcpy(dev, ifr.ifr_name);
	return fd;
}

void hex_dump(const unsigned char *buf, int len)
{
	int i, j;

	for (i=0;i < len; i += 16){
		printf("%04x  ", i);

		for (j = 0; j<16;j++){
			if (i + j< len) printf("%02x ", buf[i + j]);
			else		printf("  ");
			if (j ==7) printf(" ");
		}

		printf("|");
		for (j =0;j < 16 && i + j < len; j++){
			unsigned char c = buf[i + j];
			printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
		}

		printf("|\n");
	}
}

int msg_encode(unsigned char *buf, size_t cap, const struct msg_header *h)
{
	struct msg_header w = *h;

	if (cap < sizeof(w)){
		return -1;
	}

	w.reserved[0] = w.reserved[1] = 0;
	w.session_idx = htonl(h->session_idx);
	memcpy(buf, &w, sizeof(w));

	return sizeof(w);
}

int msg_decode(const unsigned char *buf, size_t len, struct msg_header *h)
{
	if (len < sizeof(*h)){
		return -1;
	}

	memcpy(h, buf, sizeof(*h));
	h->session_idx = ntohl(h->session_idx);

	return sizeof(*h);
}

int data_encode(unsigned char *buf, size_t cap, const struct data_header *d)
{
	uint64_t v = htobe64(d->counter);

	if (cap < sizeof(v)){
		return -1;
	}

	memcpy(buf, &v, sizeof(v));

	return sizeof(v);
}

int data_decode(const unsigned char *buf, size_t len, struct data_header *d)
{
	uint64_t v;

	if (len < sizeof(v)){
		return -1;
	}

	memcpy(&v, buf, sizeof(v));
	d->counter = be64toh(v);

	return sizeof(v);
}


