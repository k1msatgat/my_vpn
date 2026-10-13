#include <string.h>

#include "ipc.h"

int ipc_send(HANDLE pipe, uint32_t type, const void *body, uint32_t len,
		DWORD timeout_ms, HANDLE cancel)
{
	uint8_t msg[IPC_MAX_MSG];
	ipc_header_t h;
	OVERLAPPED ov;
	HANDLE waits[2];
	DWORD written = 0;
	DWORD nwaits = 1;
	DWORD w;
	int ret = -1;

	if (len > sizeof(msg) - sizeof(h)) {
		return -1;
	}

	/* 머리와 내용을 한 번에 쓴다. 메시지 모드에서는 WriteFile 한 번이 메시지 하나라,
	 * 둘로 나눠 쓰면 받는 쪽에는 메시지 두 개로 보인다. */
	h.type = type;
	h.len = len;
	memcpy(msg, &h, sizeof(h));
	if (len > 0) {
		memcpy(msg + sizeof(h), body, len);
	}

	memset(&ov, 0, sizeof(ov));
	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!ov.hEvent) {
		return -1;
	}

	if (WriteFile(pipe, msg, (DWORD)sizeof(h) + len, &written, &ov)) {
		ret = 0;
		goto out;
	}
	if (GetLastError() != ERROR_IO_PENDING) {
		goto out;
	}

	waits[0] = ov.hEvent;
	if (cancel) {
		waits[nwaits++] = cancel;
	}

	w = WaitForMultipleObjects(nwaits, waits, FALSE, timeout_ms);
	if (w != WAIT_OBJECT_0) {
		/* 아직 나가지 못한 쓰기를 거둔다. 거두지 않고 돌아가면 커널이 이 함수의 스택에
		 * 있던 OVERLAPPED 에 나중에 결과를 적는다. */
		CancelIoEx(pipe, &ov);
		GetOverlappedResult(pipe, &ov, &written, TRUE);
		goto out;
	}

	if (GetOverlappedResult(pipe, &ov, &written, FALSE)) {
		ret = 0;
	}

out:
	CloseHandle(ov.hEvent);
	SecureZeroMemory(msg, sizeof(msg));	/* 등록 코드나 키가 지나갔을 수 있다 */

	return ret;
}
