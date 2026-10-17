#pragma once

#define IDD_MAIN		100
#define IDD_DIAG		103
#define IDD_SETTINGS		104
#define IDI_APP			110

/* 연결 (서버 / 포트 / 어댑터 칸은 설정 창에 있다) */
#define IDC_ADAPTER		1001
#define IDC_SERVER		1002
#define IDC_PORT		1003
#define IDC_CONNECT		1005

/* 패킷 / 로그 */
#define IDC_TAB			1010
#define IDC_PACKETS		1011
#define IDC_LOG			1012
#define IDC_DETAIL		1013
#define IDC_POSTURE		1014
#define IDC_ENROLL		1015
#define IDC_DIAG		1016
#define IDC_SETTINGS		1017

/* 기기 등록 창 */
#define IDD_ENROLL		101
#define IDC_ENROLL_ID		1050
#define IDC_ENROLL_KEY		1051
#define IDC_ENROLL_COPY		1052

#define IDD_ENROLL_CODE		102
#define IDC_ENROLL_CODE		1053
#define IDC_ENROLL_MANUAL	1054

/* 필터와 도구 */
#define IDC_FILTER_DIR		1020
#define IDC_HIDE_KA		1021
#define IDC_ONLY_DROPS		1022
#define IDC_CLEAR		1023
#define IDC_SAVE		1024

#ifndef IDC_STATIC
#define IDC_STATIC		(-1)
#endif
