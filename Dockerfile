# ztgate 서버와 Linux 클라이언트를 한 이미지에 담는다. 첫 인자로 고른다.
#
#   docker build -t ztgate .
#   docker run --cap-add NET_ADMIN --device /dev/net/tun -p 9000:9000/udp \
#       -v ztgate_etc:/etc/ztgate ztgate server
#   docker run --cap-add NET_ADMIN --device /dev/net/tun -e SERVER=<ip> -e ENROLL_CODE=<code> \
#       -v ztgate_id:/var/lib/ztgate ztgate client
#
# --privileged 는 쓰지 않는다. tun 을 만들고 주소 / 경로 / nft 를 다루는 데 필요한 것은
# CAP_NET_ADMIN 과 /dev/net/tun 뿐이다. compose 로 서버 + 클라이언트를 돌리려면 docker/demo.sh.

# ---- 빌드 --------------------------------------------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update \
	&& apt-get install -y --no-install-recommends gcc make libc6-dev libssl-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY lib lib
COPY server server
COPY client client
RUN make CFLAGS="-Wall -Wextra -Werror -O2" server client \
	&& ./server/bin/vpn_server selftest > /dev/null \
	&& ./client/bin/vpn_client selftest > /dev/null

# ---- 실행 --------------------------------------------------------------------
# libssl3: 세션 암호화, iproute2: 서버 tun 에 주소 붙이기, nftables: 클라이언트의 방화벽 포스처,
# openssl: 등록 코드에서 임시 신원 뽑기(enroll-code.sh), procps: 서버에 SIGHUP (pkill),
# iputils-ping: 터널 확인.
FROM debian:bookworm-slim

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		libssl3 iproute2 nftables openssl procps iputils-ping \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=build /src/server/bin/vpn_server /src/client/bin/vpn_client /usr/local/bin/
COPY docker/entrypoint.sh docker/enroll-code.sh /usr/local/bin/

# 서버의 기기 목록 / ACL / 등록 대기 목록 / 감사 로그, 클라이언트의 신원. 컨테이너를 다시
# 만들어도 남아야 하는 것들이다.
VOLUME ["/etc/ztgate", "/var/lib/ztgate"]
EXPOSE 9000/udp

ENTRYPOINT ["entrypoint.sh"]
CMD ["server"]
