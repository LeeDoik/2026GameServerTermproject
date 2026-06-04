#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <WinSock2.h>
#include <winsock.h>
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <array>
#include <memory>
#include <string>

using namespace std;
using namespace chrono;

extern HWND		hWnd;

const static int MAX_TEST = 5000;   // 목표 동접 (채점 기준 5000 CCU. 도달 시 추가 접속 중단, 측정 지연이 높으면 자동 백오프)
const static int MAX_CLIENTS = MAX_TEST * 2;
const static int INVALID_ID = -1;
const static int MAX_PACKET_SIZE = 255;
const static int MAX_BUFF_SIZE = 255;

#pragma comment (lib, "ws2_32.lib")

#include "..\..\MMOSERVER_Termproject\MMOSERVER_Termproject\protocol_2026.h"

HANDLE g_hiocp;

enum OPTYPE { OP_SEND, OP_RECV, OP_DO_MOVE };

high_resolution_clock::time_point last_connect_time;

struct OverlappedEx {
	WSAOVERLAPPED over;
	WSABUF wsabuf;
	unsigned char IOCP_buf[MAX_BUFF_SIZE];
	OPTYPE event_type;
	int event_target;
};

struct CLIENT {
	int id;
	int x;
	int y;
	atomic_bool connected;

	SOCKET client_socket;
	OverlappedEx recv_over;
	unsigned char packet_buf[MAX_PACKET_SIZE];
	int prev_packet_data;
	int curr_packet_size;
	high_resolution_clock::time_point last_move_time;
};

array<int, MAX_CLIENTS> client_map;
array<CLIENT, MAX_CLIENTS> g_clients;
atomic_int num_connections;
atomic_int client_to_close;
atomic_int active_clients;

int			global_delay;				// ms����, 1000�� ������ Ŭ���̾�Ʈ ���� ����
char		g_server_ip[64];			// ������ ���� IP �ּ�

vector <thread*> worker_threads;
thread test_thread;

float point_cloud[MAX_TEST * 2];

// ���߿� NPC���� �߰� Ȯ�� ��
struct ALIEN {
	int id;
	int x, y;
	int visible_count;
};

void error_display(const char* msg, int err_no)
{
	// 진단 출력은 측정 스레드를 막지 않도록 모든 스레드 합산 초당 1회로 제한.
	// (5000 CCU 부근에서 WSAConnect 실패가 폭주하면 매 실패마다 동기 콘솔 flush가
	//  Test_Thread를 멈춰 측정 지연(global_delay)이 부풀려지던 문제 수정.
	//  FormatMessage 시스템콜 자체도 비싸므로 throttle 통과 시에만 호출한다.)
	static std::atomic<long long> last_err_ms{ 0 };
	long long now_ms = duration_cast<milliseconds>(
		high_resolution_clock::now().time_since_epoch()).count();
	long long prev = last_err_ms.load(std::memory_order_relaxed);
	if (now_ms - prev < 1000) return;                                   // 1초 내 중복 진단은 버림
	if (!last_err_ms.compare_exchange_strong(prev, now_ms,
		std::memory_order_relaxed)) return;                             // 다른 스레드가 먼저 출력

	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::cout << msg;
	std::wcout << L"Error: " << lpMsgBuf << L'\n';

	// MessageBox 팝업은 스트레스 테스트를 블로킹하므로 제거 — 콘솔 출력만 유지
	LocalFree(lpMsgBuf);
}

void DisconnectClient(int ci)
{
	bool status = true;
	if (true == atomic_compare_exchange_strong(&g_clients[ci].connected, &status, false)) {
		closesocket(g_clients[ci].client_socket);
		active_clients--;
	}
	// cout << "Client [" << ci << "] Disconnected!\n";
}

void SendPacket(int cl, void* packet)
{
	int psize = reinterpret_cast<unsigned char*>(packet)[0];
	int ptype = reinterpret_cast<unsigned char*>(packet)[1];
	OverlappedEx* over = new OverlappedEx;
	over->event_type = OP_SEND;
	memcpy(over->IOCP_buf, packet, psize);
	ZeroMemory(&over->over, sizeof(over->over));
	over->wsabuf.buf = reinterpret_cast<CHAR*>(over->IOCP_buf);
	over->wsabuf.len = psize;
	int ret = WSASend(g_clients[cl].client_socket, &over->wsabuf, 1, NULL, 0,
		&over->over, NULL);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("Error in SendPacket:", err_no);
	}
	// std::cout << "Send Packet [" << ptype << "] To Client : " << cl << std::endl;
}

void ProcessPacket(int ci, unsigned char packet[])
{
	switch (packet[1]) {
	case S2C_LOGIN_RESULT:
	{
		S2C_LoginResult* p = reinterpret_cast<S2C_LoginResult*>(packet);
		if (p->success) {

			C2S_Login l_packet;

			int temp = num_connections;
			sprintf_s(l_packet.username, "%d", temp);
			l_packet.size = sizeof(l_packet);
			l_packet.type = C2S_LOGIN;
			SendPacket(ci, &l_packet);
		}
		else {
			DisconnectClient(ci);
			//g_window->close();
		}
		break;  // (이전 fall-through 버그 수정)
	}
	case S2C_MOVE_OBJECT: {
		S2C_MoveObject* move_packet = reinterpret_cast<S2C_MoveObject*>(packet);
		if (move_packet->object_id < MAX_CLIENTS) {
			int my_id = client_map[move_packet->object_id];
			if (-1 != my_id) {
				g_clients[my_id].x = move_packet->x;
				g_clients[my_id].y = move_packet->y;
			}
			if (ci == my_id) {
				if (0 != move_packet->move_time) {
					auto d_ms = duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count() - move_packet->move_time;

					if (global_delay < d_ms) global_delay++;
					else if (global_delay > d_ms) global_delay--;
				}
			}
		}
	}
					   break;
	case S2C_ADD_OBJECT: break;
	case S2C_REMOVE_OBJECT: break;
	case S2C_CHAT_MESSAGE: break;
	case S2C_STATUS_CHANGE: break;
	// Stage 5에 추가된 패킷들 — 스트레스 더미 클라는 시각 효과 / 데미지 무시
	case S2C_ATTACK_ANIM: break;
	case S2C_DAMAGE: break;
	case S2C_DEATH: break;
	case S2C_RESPAWN: break;
	case S2C_LEVEL_UP: break;
	case S2C_SKILL_EFFECT: break;  // Stage 7: 스킬 이펙트 — 더미 클라는 무시
	case S2C_PARTY_INVITED: break;  // Stage 7 파티 — 더미 클라는 무시
	case S2C_PARTY_UPDATE: break;
	case S2C_ITEM_DROP: break;     // Stage 8 아이템 — 더미 클라는 무시
	case S2C_ITEM_REMOVE: break;
	case S2C_INVENTORY: break;
	case S2C_QUEST_DIALOGUE: break;  // Stage 9 퀘스트 — 더미 클라는 무시
	case S2C_QUEST_UPDATE: break;
	case S2C_AVATAR_INFO:
	{
		g_clients[ci].connected = true;
		active_clients++;
		S2C_AvatarInfo* login_packet = reinterpret_cast<S2C_AvatarInfo*>(packet);
		int my_id = ci;
		client_map[login_packet->playerId] = my_id;
		g_clients[my_id].id = login_packet->playerId;
		g_clients[my_id].x = login_packet->x;
		g_clients[my_id].y = login_packet->y;

		// 5000 CCU 대표성: 접속 직후 더미를 맵 전역 랜덤 좌표로 분산.
		// (서버는 신규 캐릭을 80x80 마을에 스폰 → 수천 더미가 한곳에 몰리면 시야 겹침으로 O(n^2)
		//  이동 브로드캐스트가 폭주. 실제 5000 동접은 2000x2000 전역에 흩어져 있으므로 텔레포트로 분산)
		C2S_Teleport t_packet;
		t_packet.size = sizeof(t_packet);
		t_packet.type = C2S_TELEPORT;
		t_packet.x = static_cast<short>(rand() % WORLD_WIDTH);
		t_packet.y = static_cast<short>(rand() % WORLD_HEIGHT);
		SendPacket(my_id, &t_packet);
		g_clients[my_id].x = t_packet.x;
		g_clients[my_id].y = t_packet.y;
	}
	break;
	default: break;  // 알 수 없는 패킷은 조용히 무시 (기존: MessageBox+무한루프 → 테스트 전체 정지)
	}
}

void Worker_Thread()
{
	while (true) {
		DWORD io_size;
		unsigned long long ci;
		OverlappedEx* over;
		BOOL ret = GetQueuedCompletionStatus(g_hiocp, &io_size, &ci,
			reinterpret_cast<LPWSAOVERLAPPED*>(&over), INFINITE);
		// std::cout << "GQCS :";
		int client_id = static_cast<int>(ci);
		if (FALSE == ret) {
			int err_no = WSAGetLastError();
			if (64 == err_no) DisconnectClient(client_id);
			else {
				// error_display("GQCS : ", WSAGetLastError());
				DisconnectClient(client_id);
			}
			if (OP_SEND == over->event_type) delete over;
		}
		if (0 == io_size) {
			DisconnectClient(client_id);
			continue;
		}
		if (OP_RECV == over->event_type) {
			//std::cout << "RECV from Client :" << ci;
			//std::cout << "  IO_SIZE : " << io_size << std::endl;
			unsigned char* buf = g_clients[ci].recv_over.IOCP_buf;
			unsigned psize = g_clients[ci].curr_packet_size;
			unsigned pr_size = g_clients[ci].prev_packet_data;
			while (io_size > 0) {
				if (0 == psize) psize = buf[0];
				if (io_size + pr_size >= psize) {
					// ���� ��Ŷ �ϼ� ����
					unsigned char packet[MAX_PACKET_SIZE];
					memcpy(packet, g_clients[ci].packet_buf, pr_size);
					memcpy(packet + pr_size, buf, psize - pr_size);
					ProcessPacket(static_cast<int>(ci), packet);
					io_size -= psize - pr_size;
					buf += psize - pr_size;
					psize = 0; pr_size = 0;
				}
				else {
					memcpy(g_clients[ci].packet_buf + pr_size, buf, io_size);
					pr_size += io_size;
					io_size = 0;
				}
			}
			g_clients[ci].curr_packet_size = psize;
			g_clients[ci].prev_packet_data = pr_size;
			DWORD recv_flag = 0;
			int ret = WSARecv(g_clients[ci].client_socket,
				&g_clients[ci].recv_over.wsabuf, 1,
				NULL, &recv_flag, &g_clients[ci].recv_over.over, NULL);
			if (SOCKET_ERROR == ret) {
				int err_no = WSAGetLastError();
				if (err_no != WSA_IO_PENDING)
				{
					//error_display("RECV ERROR", err_no);
					DisconnectClient(client_id);
				}
			}
		}
		else if (OP_SEND == over->event_type) {
			if (io_size != over->wsabuf.len) {
				// std::cout << "Send Incomplete Error!\n";
				DisconnectClient(client_id);
			}
			delete over;
		}
		else if (OP_DO_MOVE == over->event_type) {
			// Not Implemented Yet
			delete over;
		}
		else {
			std::cout << "Unknown GQCS event!\n";
			while (true);
		}
	}
}

constexpr int DELAY_LIMIT = 100;
constexpr int DELAY_LIMIT2 = 150;
constexpr int ACCEPT_DELY = 50;

void Adjust_Number_Of_Client()
{
	static int delay_multiplier = 1;
	static int max_limit = MAXINT;
	static bool increasing = true;

	if (active_clients >= MAX_TEST) return;
	if (num_connections >= MAX_CLIENTS) return;

	auto duration = high_resolution_clock::now() - last_connect_time;
	if (ACCEPT_DELY * delay_multiplier > duration_cast<milliseconds>(duration).count()) return;

	int t_delay = global_delay;
	if (DELAY_LIMIT2 < t_delay) {
		if (true == increasing) {
			max_limit = active_clients;
			increasing = false;
		}
		if (100 > active_clients) return;
		if (ACCEPT_DELY * 10 > duration_cast<milliseconds>(duration).count()) return;
		last_connect_time = high_resolution_clock::now();
		DisconnectClient(client_to_close);
		client_to_close++;
		return;
	}
	else
		if (DELAY_LIMIT < t_delay) {
			delay_multiplier = 10;
			return;
		}
	if (max_limit - (max_limit / 20) < active_clients) return;

	increasing = true;
	last_connect_time = high_resolution_clock::now();
	g_clients[num_connections].client_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	// Nagle 비활성화: 서버와 동일하게 작은 move 패킷이 묶이지 않도록.
	// VPN/원거리 RTT 링크에서 측정 지연(global_delay)이 실제 서버 처리시간이 아니라
	// Nagle 지연으로 부풀려지는 것을 방지한다.
	{
		BOOL nodelay = TRUE;
		setsockopt(g_clients[num_connections].client_socket, IPPROTO_TCP, TCP_NODELAY,
			reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
	}

	SOCKADDR_IN ServerAddr;
	ZeroMemory(&ServerAddr, sizeof(SOCKADDR_IN));
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_port = htons(PORT);
	ServerAddr.sin_addr.s_addr = inet_addr(g_server_ip);


	int Result = WSAConnect(g_clients[num_connections].client_socket, (sockaddr*)&ServerAddr, sizeof(ServerAddr), NULL, NULL, NULL, NULL);
	if (0 != Result) {
		error_display("WSAConnect : ", WSAGetLastError());
	}

	g_clients[num_connections].curr_packet_size = 0;
	g_clients[num_connections].prev_packet_data = 0;
	ZeroMemory(&g_clients[num_connections].recv_over, sizeof(g_clients[num_connections].recv_over));
	g_clients[num_connections].recv_over.event_type = OP_RECV;
	g_clients[num_connections].recv_over.wsabuf.buf =
		reinterpret_cast<CHAR*>(g_clients[num_connections].recv_over.IOCP_buf);
	g_clients[num_connections].recv_over.wsabuf.len = sizeof(g_clients[num_connections].recv_over.IOCP_buf);

	DWORD recv_flag = 0;
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_clients[num_connections].client_socket), g_hiocp, num_connections, 0);

	int ret = WSARecv(g_clients[num_connections].client_socket, &g_clients[num_connections].recv_over.wsabuf, 1,
		NULL, &recv_flag, &g_clients[num_connections].recv_over.over, NULL);
	if (SOCKET_ERROR == ret) {
		int err_no = WSAGetLastError();
		if (err_no != WSA_IO_PENDING)
		{
			error_display("RECV ERROR", err_no);
			goto fail_to_connect;
		}
	}
	num_connections++;
fail_to_connect:
	return;
}

void Test_Thread()
{
	while (true) {
		//Sleep(max(20, global_delay));
		Adjust_Number_Of_Client();

		// 헤드리스 측정용: 초당 1회 동접/지연 stdout 출력
		{
			static high_resolution_clock::time_point last_log = high_resolution_clock::now();
			auto now = high_resolution_clock::now();
			if (duration_cast<milliseconds>(now - last_log).count() >= 1000) {
				last_log = now;
				std::cout << "[Stress] active=" << active_clients
				          << " conns=" << num_connections
				          << " delay=" << global_delay << "ms" << std::endl;
			}
		}

		for (int i = 0; i < num_connections; ++i) {
			if (false == g_clients[i].connected) continue;
			if (g_clients[i].last_move_time + 1s > high_resolution_clock::now()) continue;
			g_clients[i].last_move_time = high_resolution_clock::now();
			C2S_Move my_packet;
			my_packet.size = sizeof(my_packet);
			my_packet.type = C2S_MOVE;
			short nx = static_cast<short>(g_clients[i].x);
			short ny = static_cast<short>(g_clients[i].y);
			switch (rand() % 4) {
			case 0: if (ny > 0) ny--; break;
			case 1: if (ny < WORLD_HEIGHT - 1) ny++; break;
			case 2: if (nx > 0) nx--; break;
			case 3: if (nx < WORLD_WIDTH - 1) nx++; break;
			}
			my_packet.x = nx;
			my_packet.y = ny;
			my_packet.move_time = static_cast<int>(duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count());
			SendPacket(i, &my_packet);
		}
	}
}

void InitializeNetwork()
{
	std::cout << "Server IP (default 127.0.0.1): ";
	std::string input_ip;
	std::getline(std::cin, input_ip);
	if (input_ip.empty())
		strncpy_s(g_server_ip, "127.0.0.1", sizeof(g_server_ip) - 1);
	else
		strncpy_s(g_server_ip, input_ip.c_str(), sizeof(g_server_ip) - 1);
	std::cout << "Server IP: " << g_server_ip << std::endl;

	for (auto& cl : g_clients) {
		cl.connected = false;
		cl.id = INVALID_ID;
	}

	for (auto& cl : client_map) cl = -1;
	num_connections = 0;
	last_connect_time = high_resolution_clock::now();

	WSADATA	wsadata;
	WSAStartup(MAKEWORD(2, 2), &wsadata);

	g_hiocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, NULL, 0);

	for (int i = 0; i < 6; ++i)
		worker_threads.push_back(new std::thread{ Worker_Thread });

	test_thread = thread{ Test_Thread };
}

void ShutdownNetwork()
{
	test_thread.join();
	for (auto pth : worker_threads) {
		pth->join();
		delete pth;
	}
}

void Do_Network()
{
	return;
}

void GetPointCloud(int* size, float** points)
{
	int index = 0;
	for (int i = 0; i < num_connections; ++i)
		if (true == g_clients[i].connected) {
			point_cloud[index * 2] = static_cast<float>(g_clients[i].x);
			point_cloud[index * 2 + 1] = static_cast<float>(g_clients[i].y);
			index++;
		}

	*size = index;
	*points = point_cloud;
}

