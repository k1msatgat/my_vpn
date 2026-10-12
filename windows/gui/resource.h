#pragma once

#define IDD_MAIN		100
#define IDI_APP			110

/* 연결 설정 */
#define IDC_ADAPTER		1001
#define IDC_SERVER		1002
#define IDC_PORT		1003
#define IDC_TUNIP		1004
#define IDC_CONNECT		1005

/* 패킷 / 로그 */
#define IDC_TAB			1010
#define IDC_PACKETS		1011
#define IDC_LOG			1012
#define IDC_DETAIL		1013
#define IDC_POSTURE		1014
#define IDC_ENROLL		1015

/* 기기 등록 창 */
#define IDD_ENROLL		101
#define IDC_ENROLL_ID		1050
#define IDC_ENROLL_KEY		1051
#define IDC_ENROLL_COPY		1052

/* 필터와 도구 */
#define IDC_FILTER_DIR		1020
#define IDC_HIDE_KA		1021
#define IDC_ONLY_DROPS		1022
#define IDC_CLEAR		1023
#define IDC_SAVE		1024

/* 라벨 (OnSize 에서 위치를 잡아야 하므로 ID 를 준다).
 * 상태, 통계, 판의 제목은 컨트롤이 아니라 다이얼로그가 직접 그린다. */
#define IDC_LBL_ADAPTER		1040
#define IDC_LBL_TUNIP		1041
#define IDC_LBL_SERVER		1042
#define IDC_LBL_PORT		1043

#ifndef IDC_STATIC
#define IDC_STATIC		(-1)
#endif
