#ifndef CONNECTOR_H
#define CONNECTOR_H

/* 커넥터 — 보호할 네트워크 안에 두는 클라이언트 (server.c 의 "커넥터").
 *
 *   vpn_client connector <server-ip> <port> <lan-ifname>
 *
 * 평소의 클라이언트와 같은 세션을 연다. 다른 것은 받은 패킷의 갈 곳이다. 서버가 커넥터의
 * 세션으로 보내 준 패킷(목적지가 내부망)을 tun 에 쓰면, 커널이 lan 쪽으로 전달하고 출발지를
 * 이 장비의 주소로 바꾼다 (NAT). 내부망의 자원은 터널 대역을 몰라도 되고, 답은 이 장비로
 * 돌아와 다시 tun 으로 들어간다.
 *
 * 그러려면 이 장비에서 IP 전달을 켜고 nftables 규칙을 넣어야 한다. 규칙은 자기 표(table ip
 * my_vpn_connector) 하나에 넣고 끝날 때 통째로 지운다. 장비의 다른 규칙은 건드리지 않는다.
 *
 *   tun -> lan                    통과 (서버의 ACL 을 이미 지났다)
 *   lan -> tun, 이미 있는 연결의 답  통과
 *   lan -> tun, 그 밖               버림 — 내부망 쪽에서 터널로 연결을 시작하지 못한다
 *   tun 대역 -> lan                출발지를 이 장비의 lan 주소로 (masquerade) */

#include <netinet/in.h>

/* 이름에 쓸 수 있는 글자만 있는가. nft 규칙 안에 그대로 들어가는 문자열이다. */
int connector_ifname_ok(const char *name);

/* 전달과 규칙을 켠다. tun_net / prefix 는 터널 대역 (서버가 준 주소에서). 성공 0. */
int connector_up(const char *tun_if, const char *lan_if, struct in_addr tun_net, int prefix);

/* 켠 것을 되돌린다. 켜지 않았으면 아무것도 하지 않는다. */
void connector_down(void);

#endif
