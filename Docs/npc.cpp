#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <tbb/concurrent_unordered_map.h>
#include <concurrent_priority_queue.h>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <chrono>
#include "protocol.h"

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")

using namespace std;
using namespace std::chrono;

constexpr int BUF_SIZE = 200;
constexpr int VIEW_RANGE = 5;
constexpr int MOVE_COOL_TIME = 1000;

constexpr int SECTOR_SIZE = 10;
constexpr int NUM_SECTORS_X = WORLD_WIDTH / SECTOR_SIZE;
constexpr int NUM_SECTORS_Y = WORLD_HEIGHT / SECTOR_SIZE;

struct Sector {
	std::mutex m_lock;
	std::unordered_set<int> players;
	std::unordered_set<int> npcs;
};
Sector g_sectors[NUM_SECTORS_Y][NUM_SECTORS_X];

constexpr int EVENT_MOVE = 1;

struct EventItem {
	int obj_id;
	system_clock::time_point wakeup_time;
	int event_id;
	bool operator<(const EventItem& o) const {
		return wakeup_time > o.wakeup_time;
	}
};
concurrency::concurrent_priority_queue<EventItem> g_timer_queue;

enum IOType { IO_SEND, IO_RECV, IO_ACCEPT, IO_NPCMOVE };

class EXP_OVER {
public:
	WSAOVERLAPPED m_over;
	IOType        m_iotype;
	WSABUF        m_wsa;
	SOCKET        m_client_socket;
	char          m_buff[BUF_SIZE];

	EXP_OVER() {
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
		m_client_socket = INVALID_SOCKET;
		m_iotype = IO_RECV;
	}
	EXP_OVER(IOType iot) : m_iotype(iot) {
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
		m_client_socket = INVALID_SOCKET;
	}
};

enum CL_STATE { CS_CONNECT, CS_PLAYING, CS_LOGOUT };
bool is_npc(int id);
bool is_pc(int id);

class SESSION {
public:
	SOCKET   m_client;
	int      m_id;
	CL_STATE m_state;
	EXP_OVER m_recv_over;
	int      m_prev_recv;
	char     m_username[MAX_NAME_LEN];
	std::atomic<short> m_x, m_y;
	int      m_move_time;

	std::unordered_set<int> m_view_list;
	std::mutex              m_vl_mutex;

	std::atomic<bool>        m_active_npc;
	system_clock::time_point m_npc_last_move_time;

	SESSION() {
		std::cout << "SESSION Creation Error!" << std::endl;
		exit(-1);
	}

	SESSION(SOCKET s, int id) : m_client(s), m_id(id) {
		m_state = CS_CONNECT;
		m_recv_over.m_iotype = IO_RECV;
		m_x = static_cast<short>(rand() % WORLD_WIDTH);
		m_y = static_cast<short>(rand() % WORLD_HEIGHT);
		m_prev_recv = 0;
		m_move_time = 0;
		m_active_npc = false;
		m_npc_last_move_time = system_clock::now();
	}

	SESSION(int id, short x, short y, const char* name)
		: m_client(INVALID_SOCKET), m_id(id)
	{
		m_state = CS_PLAYING;
		m_recv_over.m_iotype = IO_RECV;
		m_x = x;
		m_y = y;
		m_prev_recv = 0;
		m_move_time = 0;
		m_active_npc = false;
		m_npc_last_move_time = system_clock::now();
		strncpy_s(m_username, name, MAX_NAME_LEN);
	}

	~SESSION() {
		if (m_client != INVALID_SOCKET)
			closesocket(m_client);
	}

	bool is_visible(short ox, short oy) const {
		return abs(m_x.load() - ox) <= VIEW_RANGE
			&& abs(m_y.load() - oy) <= VIEW_RANGE;
	}

	void do_recv() {
		if (m_client == INVALID_SOCKET) return;
		DWORD recv_flag = 0;
		memset(&m_recv_over.m_over, 0, sizeof(m_recv_over.m_over));
		WSARecv(m_client, &m_recv_over.m_wsa, 1, 0, &recv_flag,
			&m_recv_over.m_over, nullptr);
	}

	void do_send(int num_bytes, char* mess) {
		if (m_client == INVALID_SOCKET) return;
		EXP_OVER* o = new EXP_OVER(IO_SEND);
		o->m_wsa.len = num_bytes;
		memcpy(o->m_buff, mess, num_bytes);
		WSASend(m_client, &o->m_wsa, 1, 0, 0, &o->m_over, nullptr);
	}

	void send_avatar_info() {
		S2C_AvatarInfo packet;
		packet.size     = sizeof(S2C_AvatarInfo);
		packet.type     = S2C_AVATAR_INFO;
		packet.playerId = m_id;
		packet.x        = m_x;
		packet.y        = m_y;
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}

	void send_login_success() {
		S2C_LoginResult packet;
		packet.size    = sizeof(S2C_LoginResult);
		packet.type    = S2C_LOGIN_RESULT;
		packet.success = true;
		strncpy_s(packet.message, "Login successful.", sizeof(packet.message));
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}

	void send_move_packet(int mover);
	void send_add_player(int player_id);
	void send_remove_player(int player_id);
	void wake_up();
	void do_random_move();
	bool process_packet(unsigned char* p);
};

tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<SESSION>>> clients;
SOCKET  g_server;
HANDLE  g_iocp;
std::atomic<int> player_index = 1;

bool is_npc(int id) { return id >= NPC_ID_START; }
bool is_pc(int id)  { return id < NPC_ID_START; }

void SESSION::send_add_player(int player_id)
{
	S2C_AddPlayer packet;
	packet.size     = sizeof(S2C_AddPlayer);
	packet.type     = S2C_ADD_PLAYER;
	packet.playerId = player_id;
	std::shared_ptr<SESSION> pl = clients[player_id].load();
	if (nullptr == pl) return;
	memcpy(packet.username, pl->m_username, sizeof(packet.username));
	packet.x = pl->m_x;
	packet.y = pl->m_y;
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::send_remove_player(int player_id)
{
	S2C_RemovePlayer packet;
	packet.size     = sizeof(S2C_RemovePlayer);
	packet.type     = S2C_REMOVE_PLAYER;
	packet.playerId = player_id;
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::send_move_packet(int mover)
{
	S2C_MovePlayer packet;
	packet.size     = sizeof(S2C_MovePlayer);
	packet.type     = S2C_MOVE_PLAYER;
	packet.playerId = mover;
	std::shared_ptr<SESSION> pl = clients[mover].load();
	if (nullptr == pl) return;
	packet.x         = pl->m_x;
	packet.y         = pl->m_y;
	packet.move_time = pl->m_move_time;
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::wake_up()
{
	bool expected = false;
	if (!m_active_npc.compare_exchange_strong(expected, true))
		return;
	EventItem ev;
	ev.obj_id      = m_id;
	ev.event_id    = EVENT_MOVE;
	ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
	g_timer_queue.push(ev);
}

void SESSION::do_random_move()
{
	short old_x = m_x;
	short old_y = m_y;
	int old_sx = max(0, min(NUM_SECTORS_X - 1, (int)old_x / SECTOR_SIZE));
	int old_sy = max(0, min(NUM_SECTORS_Y - 1, (int)old_y / SECTOR_SIZE));

	std::unordered_set<int> old_vl;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int nx = old_sx + dx, ny = old_sy + dy;
			if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
			std::lock_guard<std::mutex> lock(g_sectors[ny][nx].m_lock);
			for (int pid : g_sectors[ny][nx].players) {
				std::shared_ptr<SESSION> pl = clients[pid].load();
				if (pl && pl->m_state == CS_PLAYING && is_visible(pl->m_x, pl->m_y))
					old_vl.insert(pid);
			}
		}
	}

	switch (rand() % 4) {
	case 0: if (m_x < WORLD_WIDTH  - 1) m_x++; break;
	case 1: if (m_x > 0)                m_x--; break;
	case 2: if (m_y < WORLD_HEIGHT - 1) m_y++; break;
	case 3: if (m_y > 0)                m_y--; break;
	}

	int new_sx = max(0, min(NUM_SECTORS_X - 1, m_x.load() / SECTOR_SIZE));
	int new_sy = max(0, min(NUM_SECTORS_Y - 1, m_y.load() / SECTOR_SIZE));

	if (old_sx != new_sx || old_sy != new_sy) {
		{
			std::lock_guard<std::mutex> lock(g_sectors[old_sy][old_sx].m_lock);
			g_sectors[old_sy][old_sx].npcs.erase(m_id);
		}
		{
			std::lock_guard<std::mutex> lock(g_sectors[new_sy][new_sx].m_lock);
			g_sectors[new_sy][new_sx].npcs.insert(m_id);
		}
	}

	std::unordered_set<int> new_vl;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int nx = new_sx + dx, ny = new_sy + dy;
			if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
			std::lock_guard<std::mutex> lock(g_sectors[ny][nx].m_lock);
			for (int pid : g_sectors[ny][nx].players) {
				std::shared_ptr<SESSION> pl = clients[pid].load();
				if (pl && pl->m_state == CS_PLAYING && is_visible(pl->m_x, pl->m_y))
					new_vl.insert(pid);
			}
		}
	}

	for (int pid : new_vl) {
		std::shared_ptr<SESSION> pl = clients[pid].load();
		if (!pl) continue;
		if (old_vl.count(pid) == 0) {
			{
				std::lock_guard<std::mutex> lock(pl->m_vl_mutex);
				pl->m_view_list.insert(m_id);
			}
			pl->send_add_player(m_id);
		}
		else {
			pl->send_move_packet(m_id);
		}
	}
	for (int pid : old_vl) {
		if (new_vl.count(pid) == 0) {
			std::shared_ptr<SESSION> pl = clients[pid].load();
			if (!pl) continue;
			{
				std::lock_guard<std::mutex> lock(pl->m_vl_mutex);
				pl->m_view_list.erase(m_id);
			}
			pl->send_remove_player(m_id);
		}
	}

	m_npc_last_move_time = system_clock::now();
}

bool SESSION::process_packet(unsigned char* p)
{
	PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);
	switch (type) {
	case C2S_LOGIN: {
		C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);
		strncpy_s(m_username, packet->username, MAX_NAME_LEN);
		cout << "Player[" << m_id << "] logged in as " << m_username << endl;
		send_avatar_info();
		m_state = CS_PLAYING;

		int sx = max(0, min(NUM_SECTORS_X - 1, m_x.load() / SECTOR_SIZE));
		int sy = max(0, min(NUM_SECTORS_Y - 1, m_y.load() / SECTOR_SIZE));
		{
			std::lock_guard<std::mutex> lock(g_sectors[sy][sx].m_lock);
			g_sectors[sy][sx].players.insert(m_id);
		}
	}
	case C2S_MOVE: {
		if (type == C2S_MOVE) {
			C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);
			DIRECTION dir = packet->dir;
			m_move_time = packet->move_time;

			short old_x = m_x;
			short old_y = m_y;

			switch (dir) {
			case UP:    m_y = (short)max(0, m_y.load() - 1);                   break;
			case DOWN:  m_y = (short)min(WORLD_HEIGHT - 1, m_y.load() + 1);    break;
			case LEFT:  m_x = (short)max(0, m_x.load() - 1);                   break;
			case RIGHT: m_x = (short)min(WORLD_WIDTH  - 1, m_x.load() + 1);    break;
			}

			int old_sx = max(0, min(NUM_SECTORS_X - 1, old_x / SECTOR_SIZE));
			int old_sy = max(0, min(NUM_SECTORS_Y - 1, old_y / SECTOR_SIZE));
			int new_sx = max(0, min(NUM_SECTORS_X - 1, m_x.load() / SECTOR_SIZE));
			int new_sy = max(0, min(NUM_SECTORS_Y - 1, m_y.load() / SECTOR_SIZE));

			if (old_sx != new_sx || old_sy != new_sy) {
				{
					std::lock_guard<std::mutex> lock(g_sectors[old_sy][old_sx].m_lock);
					g_sectors[old_sy][old_sx].players.erase(m_id);
				}
				{
					std::lock_guard<std::mutex> lock(g_sectors[new_sy][new_sx].m_lock);
					g_sectors[new_sy][new_sx].players.insert(m_id);
				}
			}
		}

		std::unordered_set<int> near_list;
		int sx = max(0, min(NUM_SECTORS_X - 1, m_x.load() / SECTOR_SIZE));
		int sy = max(0, min(NUM_SECTORS_Y - 1, m_y.load() / SECTOR_SIZE));

		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {
				int nx = sx + dx, ny = sy + dy;
				if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
				std::lock_guard<std::mutex> lock(g_sectors[ny][nx].m_lock);
				for (int id : g_sectors[ny][nx].players) {
					if (id == m_id) continue;
					std::shared_ptr<SESSION> pl = clients[id].load();
					if (pl && pl->m_state == CS_PLAYING && is_visible(pl->m_x, pl->m_y))
						near_list.insert(id);
				}
			}
		}

		// NPC는 섹터 npcs 셋으로 탐색 (O(N) 전체 순회 제거)
		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {
				int nx = sx + dx, ny = sy + dy;
				if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
				std::lock_guard<std::mutex> lock(g_sectors[ny][nx].m_lock);
				for (int id : g_sectors[ny][nx].npcs) {
					std::shared_ptr<SESSION> npc = clients[id].load();
					if (npc && npc->m_state == CS_PLAYING && is_visible(npc->m_x, npc->m_y))
						near_list.insert(id);
				}
			}
		}

		std::unordered_set<int> my_old_vl;
		{
			std::lock_guard<std::mutex> lock(m_vl_mutex);
			my_old_vl = m_view_list;
		}

		for (int other_id : near_list) {
			bool in_my_vl = (my_old_vl.count(other_id) > 0);

			if (!in_my_vl) {
				{
					std::lock_guard<std::mutex> lock(m_vl_mutex);
					m_view_list.insert(other_id);
				}
				send_add_player(other_id);

				if (is_npc(other_id)) {
					std::shared_ptr<SESSION> npc = clients[other_id].load();
					if (npc) npc->wake_up();
				}
			}

			if (is_pc(other_id)) {
			std::shared_ptr<SESSION> other_session = clients[other_id].load();
			if (other_session && other_session->m_state == CS_PLAYING) {
					bool me_in_other_vl = false;
					{
						std::lock_guard<std::mutex> lock(other_session->m_vl_mutex);
						me_in_other_vl = (other_session->m_view_list.count(m_id) > 0);
						if (!me_in_other_vl)
							other_session->m_view_list.insert(m_id);
					}
					if (me_in_other_vl)
						other_session->send_move_packet(m_id);
					else
						other_session->send_add_player(m_id);
				}
			}
		}

		for (int other_id : my_old_vl) {
			if (near_list.count(other_id) == 0) {
				{
					std::lock_guard<std::mutex> lock(m_vl_mutex);
					m_view_list.erase(other_id);
				}
				send_remove_player(other_id);

				if (is_pc(other_id)) {
					std::shared_ptr<SESSION> other_session = clients[other_id].load();
					if (other_session) {
						bool me_in_other_vl = false;
						{
							std::lock_guard<std::mutex> lock(other_session->m_vl_mutex);
							me_in_other_vl = (other_session->m_view_list.count(m_id) > 0);
							if (me_in_other_vl)
								other_session->m_view_list.erase(m_id);
						}
						if (me_in_other_vl)
							other_session->send_remove_player(m_id);
					}
				}
			}
		}

		send_move_packet(m_id);
		break;
	}
	default:
		cout << "Unknown packet type from player[" << m_id << "]" << endl;
		return false;
	}
	return true;
}

void send_login_fail(SOCKET client, const char* message)
{
	S2C_LoginResult packet;
	packet.size    = sizeof(S2C_LoginResult);
	packet.type    = S2C_LOGIN_RESULT;
	packet.success = false;
	strncpy_s(packet.message, message, sizeof(packet.message));
	WSABUF wsa_buf;
	wsa_buf.buf = reinterpret_cast<char*>(&packet);
	wsa_buf.len = packet.size;
	WSASend(client, &wsa_buf, 1, 0, 0, nullptr, nullptr);
}

void disconnect(int key)
{
	std::cout << "client[" << key << "] Disconnected." << std::endl;
	std::shared_ptr<SESSION> cl = clients[key].load();
	if (nullptr == cl) return;

	cl->m_state = CS_LOGOUT;

	int sx = max(0, min(NUM_SECTORS_X - 1, cl->m_x.load() / SECTOR_SIZE));
	int sy = max(0, min(NUM_SECTORS_Y - 1, cl->m_y.load() / SECTOR_SIZE));
	{
		std::lock_guard<std::mutex> lock(g_sectors[sy][sx].m_lock);
		g_sectors[sy][sx].players.erase(key);
	}

	std::unordered_set<int> my_old_vl;
	{
		std::lock_guard<std::mutex> lock(cl->m_vl_mutex);
		my_old_vl = cl->m_view_list;
	}

	for (int other : my_old_vl) {
		if (is_npc(other)) continue;
		std::shared_ptr<SESSION> o = clients[other].load();
		if (!o || o->m_state != CS_PLAYING) continue;
		{
			std::lock_guard<std::mutex> lock(o->m_vl_mutex);
			o->m_view_list.erase(key);
		}
		o->send_remove_player(key);
	}

	closesocket(cl->m_client);
	cl->m_client = INVALID_SOCKET;
	clients[key].store(nullptr);
}

void worker_thread()
{
	for (;;) {
		DWORD     num_bytes;
		ULONG_PTR long_key;
		LPOVERLAPPED over;
		BOOL ret = GetQueuedCompletionStatus(g_iocp, &num_bytes, &long_key, &over, INFINITE);
		int key = static_cast<int>(long_key);

		if (TRUE != ret) {
			if (key == -1) exit(-1);
			disconnect(key);
			continue;
		}

		EXP_OVER* exp_over = reinterpret_cast<EXP_OVER*>(over);
		switch (exp_over->m_iotype) {

		case IO_ACCEPT: {
			if (player_index >= MAX_PLAYERS) {
				send_login_fail(exp_over->m_client_socket, "Server is full.");
				closesocket(exp_over->m_client_socket);
			}
			else {
				int my_id = player_index++;
				CreateIoCompletionPort((HANDLE)exp_over->m_client_socket, g_iocp, my_id, 0);
				std::shared_ptr<SESSION> new_pl =
					std::make_shared<SESSION>(exp_over->m_client_socket, my_id);
				clients[my_id].store(new_pl);
				new_pl->send_login_success();
				new_pl->do_recv();
			}
			exp_over->m_client_socket =
				WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			AcceptEx(g_server, exp_over->m_client_socket, &exp_over->m_buff, 0,
				sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
				NULL, &exp_over->m_over);
			break;
		}

		case IO_RECV: {
			if (0 == num_bytes) { disconnect(key); break; }
			std::shared_ptr<SESSION> cl = clients[key].load();
			if (!cl) break;

			unsigned char* p = reinterpret_cast<unsigned char*>(exp_over->m_buff);
			int data_size = num_bytes + cl->m_prev_recv;
			while (data_size > 0) {
				int packet_size = p[0];
				if (packet_size > data_size) break;
				if (!cl->process_packet(p)) { disconnect(key); break; }
				p         += packet_size;
				data_size -= packet_size;
			}
			if (data_size > 0) memmove(cl->m_recv_over.m_buff, p, data_size);
			cl->m_prev_recv = data_size;
			cl->do_recv();
			break;
		}

		case IO_SEND: {
			delete exp_over;
			break;
		}

		case IO_NPCMOVE: {
			delete exp_over;
			int npc_id = key;
			std::shared_ptr<SESSION> npc = clients[npc_id].load();
			if (!npc || npc->m_state != CS_PLAYING) break;

			npc->do_random_move();

			bool has_nearby = false;
			int nsx = max(0, min(NUM_SECTORS_X - 1, npc->m_x.load() / SECTOR_SIZE));
			int nsy = max(0, min(NUM_SECTORS_Y - 1, npc->m_y.load() / SECTOR_SIZE));
			for (int dy = -1; dy <= 1 && !has_nearby; ++dy) {
				for (int dx = -1; dx <= 1 && !has_nearby; ++dx) {
					int nx2 = nsx + dx, ny2 = nsy + dy;
					if (nx2 < 0 || nx2 >= NUM_SECTORS_X || ny2 < 0 || ny2 >= NUM_SECTORS_Y) continue;
					std::lock_guard<std::mutex> lock(g_sectors[ny2][nx2].m_lock);
					for (int pid : g_sectors[ny2][nx2].players) {
						std::shared_ptr<SESSION> pl = clients[pid].load();
						if (pl && pl->m_state == CS_PLAYING && npc->is_visible(pl->m_x, pl->m_y)) {
							has_nearby = true;
							break;
						}
					}
				}
			}

			if (has_nearby) {
				EventItem ev;
				ev.obj_id      = npc_id;
				ev.event_id    = EVENT_MOVE;
				ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
				g_timer_queue.push(ev);
			}
			else {
				npc->m_active_npc = false;
			}
			break;
		}

		default:
			exit(-1);
		}
	}
}

void timer_thread_local_queue()
{
	std::priority_queue<EventItem> local_queue;

	while (true) {
		{
			EventItem ev;
			while (g_timer_queue.try_pop(ev))
				local_queue.push(ev);
		}

		if (local_queue.empty()) {
			this_thread::sleep_for(milliseconds(1));
			continue;
		}

		auto now = system_clock::now();
		if (local_queue.top().wakeup_time > now) {
			auto sleep_dur = duration_cast<milliseconds>(
				local_queue.top().wakeup_time - now);
			this_thread::sleep_for(min(sleep_dur, milliseconds(1)));
			continue;
		}

		while (!local_queue.empty() && local_queue.top().wakeup_time <= now) {
			EventItem ev = local_queue.top();
			local_queue.pop();

			switch (ev.event_id) {
			case EVENT_MOVE: {
				EXP_OVER* move_over = new EXP_OVER(IO_NPCMOVE);
				PostQueuedCompletionStatus(g_iocp, 1,
					static_cast<ULONG_PTR>(ev.obj_id), &move_over->m_over);
				break;
			}
			}
		}
	}
}

void InitializeNPC()
{
	cout << "NPC initialize begin. (" << MAX_NPCS << " NPCs)" << endl;
	for (int i = 0; i < MAX_NPCS; ++i) {
		int npc_id = NPC_ID_START + i;
		short nx   = static_cast<short>(rand() % WORLD_WIDTH);
		short ny   = static_cast<short>(rand() % WORLD_HEIGHT);
		char name[MAX_NAME_LEN];
		sprintf_s(name, "NPC_%d", i);
		std::shared_ptr<SESSION> npc = std::make_shared<SESSION>(npc_id, nx, ny, name);
		clients[npc_id].store(npc);
		int sx = max(0, min(NUM_SECTORS_X - 1, (int)nx / SECTOR_SIZE));
		int sy = max(0, min(NUM_SECTORS_Y - 1, (int)ny / SECTOR_SIZE));
		g_sectors[sy][sx].npcs.insert(npc_id);
	}
	cout << "NPC initialize end." << endl;
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	g_server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family           = AF_INET;
	server_addr.sin_port             = htons(PORT);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(g_server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_server, SOMAXCONN);

	InitializeNPC();

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)g_server, g_iocp, static_cast<ULONG_PTR>(-1), 0);

	EXP_OVER accept_over(IO_ACCEPT);
	accept_over.m_client_socket =
		WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	AcceptEx(g_server, accept_over.m_client_socket, &accept_over.m_buff, 0,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &accept_over.m_over);

	vector<thread> worker_threads;
	int num_threads = thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread);

	thread timer_th(timer_thread_local_queue);

	for (auto& th : worker_threads) th.join();
	timer_th.join();

	closesocket(g_server);
	WSACleanup();
}
