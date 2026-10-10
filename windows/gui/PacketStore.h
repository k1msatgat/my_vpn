#pragma once

/* 패킷 보관 / 필터 / 집계. 다이얼로그와 분리해 두면 표시 형식과 무관하게
 * "무엇을 세고 무엇을 보여줄지"만 다룬다. 가상 리스트가 인덱스로 바로
 * 접근하므로 ViewAt() 은 O(1) 이어야 한다. */

#include <vector>

#include "tunnel.h"

#define PACKET_STORE_MAX	20000	/* 보관 상한 */
#define PACKET_STORE_TRIM	5000	/* 넘치면 한 번에 버리는 개수 */

enum PacketDirFilter {
	PKT_FILTER_ALL = 0,
	PKT_FILTER_TX,
	PKT_FILTER_RX
};

struct PacketStats {
	ULONGLONG tx_packets;		/* 실제로 보낸 것만 (송신 폐기는 제외) */
	ULONGLONG tx_bytes;
	ULONGLONG rx_packets;		/* 도착한 것 전부 (폐기 포함 — 와이어에는 왔다) */
	ULONGLONG rx_bytes;
	ULONGLONG ka_sent;
	ULONGLONG ka_recv;
	ULONGLONG rehandshakes;
	ULONGLONG lost;			/* 링버퍼가 꽉 차서 UI 가 못 받은 수 */
	ULONGLONG drops[TUNNEL_DROP_IO_ERROR + 1];
};

class CPacketStore
{
public:
	CPacketStore();

	void Add(const tunnel_packet_t &p);
	void Clear();
	void CountRehandshake();
	void SetLost(ULONGLONG n);

	/* 필터가 바뀌면 뷰를 다시 만든다. */
	void SetFilter(int dir, bool hideKeepalive, bool onlyDrops);

	int ViewCount() const;
	const tunnel_packet_t *ViewAt(int i) const;

	const PacketStats &Stats() const { return m_stats; }
	ULONGLONG TotalDrops() const;

private:
	bool Match(const tunnel_packet_t &p) const;
	void RebuildView();

	std::vector<tunnel_packet_t> m_all;
	std::vector<int> m_view;		/* m_all 의 인덱스 */
	PacketStats m_stats;
	int m_dir;
	bool m_hideKa;
	bool m_onlyDrops;
};
