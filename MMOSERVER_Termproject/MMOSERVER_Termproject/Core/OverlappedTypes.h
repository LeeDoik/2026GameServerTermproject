#pragma once

// IOCP 디스패치 시 OVERLAPPED 종류 식별. OVERLAPPED_EX와 TimerOverlapped 등
// 모든 OVERLAPPED 변형 구조체가 첫 필드로 WSAOVERLAPPED를, 두 번째 필드로 IO_TYPE을 가져야 함.
enum IO_TYPE { IO_RECV, IO_SEND, IO_ACCEPT, IO_TIMER, IO_DB_DONE };
