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

/* 사용자 로그인 (IdP 칸은 설정 창에, 로그인 / 로그아웃은 주 창의 연결 정보 판에) */
#define IDC_ISSUER		1006
#define IDC_CLIENT_ID		1007
#define IDC_SCOPE		1008
#define IDC_LOGIN		1018

/* 패킷 / 로그 */
#define IDC_TAB			1010
#define IDC_PACKETS		1011
#define IDC_LOG			1012
#define IDC_DETAIL		1013
#define IDC_ENROLL		1015
#define IDC_DIAG		1016
#define IDC_SETTINGS		1017

/* 연결된 동안의 기본 버튼 자리. 이런 컨트롤은 없다 — Enter 가 아무 일도 하지 않게 한다. */
#define IDC_NO_DEFAULT		1099

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
