#pragma once

#define IDD_MAIN		100

/* 연결 설정 */
#define IDC_ADAPTER		1001
#define IDC_SERVER		1002
#define IDC_PORT		1003
#define IDC_TUNIP		1004
#define IDC_CONNECT		1005
#define IDC_DISCONNECT		1006
#define IDC_STATE		1007
#define IDC_ELAPSED		1008

/* 패킷 / 로그 */
#define IDC_TAB			1010
#define IDC_PACKETS		1011
#define IDC_LOG			1012
#define IDC_DETAIL		1013

/* 필터와 도구 */
#define IDC_FILTER_DIR		1020
#define IDC_HIDE_KA		1021
#define IDC_ONLY_DROPS		1022
#define IDC_CLEAR		1023
#define IDC_SAVE		1024

/* 통계 */
#define IDC_STATS1		1030
#define IDC_STATS2		1031

/* 라벨 (OnSize 에서 위치를 잡아야 하므로 ID 를 준다) */
#define IDC_LBL_ADAPTER		1040
#define IDC_LBL_TUNIP		1041
#define IDC_LBL_SERVER		1042
#define IDC_LBL_PORT		1043
#define IDC_LBL_STATE		1044
#define IDC_LBL_FILTER		1045
#define IDC_GRP_CONN		1046
#define IDC_GRP_DETAIL		1047

#ifndef IDC_STATIC
#define IDC_STATIC		(-1)
#endif
