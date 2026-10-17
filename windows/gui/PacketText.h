#pragma once

/* 코어가 올려 준 패킷 이벤트(enum 과 숫자)를 사람이 읽는 글로 바꾼다.
 *
 * 화면 여러 곳이 같은 이름을 쓴다 — 주 창은 거부 사유를, 진단 창은 패킷 표와 상세를.
 * 한 곳에 두지 않으면 "포스처가 정책에 못 미침"이 창마다 다르게 적힌다. 코어는 글을
 * 만들지 않는다 (enum 만 올린다). 표시 형식은 UI 의 일이다. */

#ifndef __AFXWIN_H__
#error "include framework.h before PacketText.h"
#endif

#include "tunnel.h"

namespace pkt {

/* 진단 창 패킷 표의 열. 순서가 곧 화면의 열 순서다. */
enum Column {
	COL_SEQ = 0,
	COL_TIME,
	COL_DIR,
	COL_TYPE,
	COL_SESSION,
	COL_COUNTER,
	COL_SUMMARY,
	COL_LENGTH,
	COL_VERDICT,
	COL_COUNT
};

CString TimeText(ULONGLONG t_ms);
CString DirName(const tunnel_packet_t &p);
CString TypeName(const tunnel_packet_t &p);
CString PostureStateName(int state);
CString DenyReasonName(int reason);
CString PostureText(const posture_report_t &r);
CString VerdictName(int verdict);
CString ProtoName(const tunnel_packet_t &p);
CString Summary(const tunnel_packet_t &p);
CString ColumnText(const tunnel_packet_t &p, int col);

/* 상세창의 글: 헤더를 필드별로 되짚고 앞부분을 16진수로 보인다. */
CString DetailText(const tunnel_packet_t &p);

} /* namespace pkt */
