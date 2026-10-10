#pragma once

/* Winsock2를 MFC보다 먼저 올린다.
 * windows.h는 기본적으로 winsock.h(1.1)를 끌어오고, 그 뒤에 winsock2.h가 들어오면
 * 같은 심볼이 두 번 정의되어 깨진다. MFC 헤더도 내부에서 windows.h를 포함하므로
 * winsock2.h가 MFC보다 앞에 있어야 안전하다. */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <afxwin.h>
#include <afxext.h>
#include <afxcmn.h>		/* 공용 컨트롤 (INITCOMMONCONTROLSEX) */
#include <afxdialogex.h>
