#define NOMINMAX
#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <algorithm>
#include <winsock2.h>
#include <mswsock.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <tbb/concurrent_hash_map.h>
#include "protocol_2026.h"
#include "Core/ObjectPool.h"
#include "Core/Entity.h"
#include "Core/OverlappedTypes.h"
#include "Core/TimerManager.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

using namespace std;

// 링 버퍼 클래스 전방 선언
class RingBuffer;

struct OVERLAPPED_EX {
    WSAOVERLAPPED overlapped;
    IO_TYPE type;
    WSABUF wsa_buf;
    SOCKET client_socket;
    unsigned char buffer[MAX_CHAT_MSG_LEN + 256];

    OVERLAPPED_EX() {
        memset(&overlapped, 0, sizeof(overlapped));
        type = IO_RECV;
        wsa_buf.buf = reinterpret_cast<char*>(buffer);
        wsa_buf.len = sizeof(buffer);
        client_socket = INVALID_SOCKET;
    }
    OVERLAPPED_EX(IO_TYPE t) : OVERLAPPED_EX() { type = t; }
};

// IO_SEND 전용 OVERLAPPED_EX 풀. do_send마다 new/delete 비용 제거
ObjectPool<OVERLAPPED_EX> g_send_pool;

// Timer 만기 이벤트를 IOCP로 post할 때 사용하는 OVERLAPPED 풀
ObjectPool<TimerOverlapped> g_timer_pool;

// 전역 타이머 매니저. main에서 Start, worker_thread가 IO_TIMER로 처리
TimerManager g_timer_manager;

// --- 링 버퍼 (Ring Buffer) 구현 ---
// 기존의 prev_recv 방식(포인터 이동)을 피하고, 원형 큐 형태로 데이터를 안전하게 적재 및 추출합니다.
class RingBuffer {
private:
    vector<unsigned char> buffer;
    int head;
    int tail;
    int capacity;
    int current_size;

public:
    RingBuffer(int size = 8192) : capacity(size), head(0), tail(0), current_size(0) {
        buffer.resize(size);
    }

    int GetStoredSize() const { return current_size; }
    int GetFreeCapacity() const { return capacity - current_size; }

    bool Write(const unsigned char* data, int size) {
        if (size > GetFreeCapacity()) return false; // 오버플로우 방지
        
        int first_chunk = min(size, capacity - tail);
        memcpy(&buffer[tail], data, first_chunk);
        
        if (size > first_chunk) {
            memcpy(&buffer[0], data + first_chunk, size - first_chunk);
        }
        
        tail = (tail + size) % capacity;
        current_size += size;
        return true;
    }

    bool Peek(unsigned char* dest, int size) const {
        if (size > current_size) return false;
        
        int first_chunk = min(size, capacity - head);
        memcpy(dest, &buffer[head], first_chunk);
        
        if (size > first_chunk) {
            memcpy(dest + first_chunk, &buffer[0], size - first_chunk);
        }
        return true;
    }

    bool Read(unsigned char* dest, int size) {
        if (!Peek(dest, size)) return false;
        
        head = (head + size) % capacity;
        current_size -= size;
        return true;
    }
};

// Entity를 상속받아 id/x/y/view_list를 공유. Player는 네트워크 세션 관련 필드 추가
struct Player : public Entity {
    SOCKET socket;
    OVERLAPPED_EX recv_overlapped;
    RingBuffer packet_buffer;
    shared_ptr<string> name;
    bool is_active;

    Player() : Entity(), socket(INVALID_SOCKET), recv_overlapped(IO_RECV), is_active(false) {
        name = make_shared<string>("Guest");
    }

    ~Player() override {
        if (socket != INVALID_SOCKET) closesocket(socket);
    }

    void do_recv() {
        DWORD flags = 0;
        DWORD recv_bytes = 0;
        memset(&recv_overlapped.overlapped, 0, sizeof(recv_overlapped.overlapped));
        recv_overlapped.wsa_buf.buf = reinterpret_cast<char*>(recv_overlapped.buffer);
        recv_overlapped.wsa_buf.len = sizeof(recv_overlapped.buffer);
        WSARecv(socket, &recv_overlapped.wsa_buf, 1, &recv_bytes, &flags, &recv_overlapped.overlapped, NULL);
    }

    void do_send(int num_bytes, void* mess) {
        OVERLAPPED_EX* ov = g_send_pool.Acquire();
        ov->type = IO_SEND;
        memset(&ov->overlapped, 0, sizeof(ov->overlapped));
        ov->wsa_buf.buf = reinterpret_cast<char*>(ov->buffer);
        ov->wsa_buf.len = num_bytes;
        memcpy(ov->buffer, mess, num_bytes);
        WSASend(socket, &ov->wsa_buf, 1, NULL, 0, &ov->overlapped, NULL);
    }
};

// --- 글로벌 변수 ---
// TBB concurrent_hash_map: 버킷 단위 락. accessor 패턴으로 find/insert/erase 모두 동시 안전
using ClientMap = tbb::concurrent_hash_map<int, std::shared_ptr<Player>>;
ClientMap g_clients;
atomic<int> g_next_id{ 0 };
HANDLE g_h_iocp;
SOCKET g_listen_socket;
LPFN_ACCEPTEX g_fp_accept_ex = nullptr;

// --- 시야 관리 (Sector) ---
constexpr int WINDOW_VIEW_SIZE = 20; // 클라이언트에 표시되는 시야 (창) 크기 20x20
constexpr int GAME_VIEW_RANGE = 15;  // 게임 속 실제 객체가 보이는 시야 15x15

constexpr int SECTOR_SIZE = 20; // 시야 범위 고려 (20x20 단위로 공간 분할, 2000 / 20 = 100 딱 떨어짐)
constexpr int NUM_SECTORS_X = WORLD_WIDTH / SECTOR_SIZE;
constexpr int NUM_SECTORS_Y = WORLD_HEIGHT / SECTOR_SIZE;

struct Sector {
    mutex m_lock;
    unordered_set<int> players;
    unordered_set<int> npcs;
};

Sector g_sectors[NUM_SECTORS_Y][NUM_SECTORS_X];

// Sector 업데이트 유틸리티 함수
void UpdateObjectSector(int id, short old_x, short old_y, short new_x, short new_y, bool is_player) {
    int old_sx = max(0, min(NUM_SECTORS_X - 1, old_x / SECTOR_SIZE));
    int old_sy = max(0, min(NUM_SECTORS_Y - 1, old_y / SECTOR_SIZE));
    int new_sx = max(0, min(NUM_SECTORS_X - 1, new_x / SECTOR_SIZE));
    int new_sy = max(0, min(NUM_SECTORS_Y - 1, new_y / SECTOR_SIZE));

    if (old_sx != new_sx || old_sy != new_sy) {
        if (old_x != -1 && old_y != -1) { // -1, -1은 초기 생성 시
            lock_guard<mutex> lock(g_sectors[old_sy][old_sx].m_lock);
            if (is_player) g_sectors[old_sy][old_sx].players.erase(id);
            else g_sectors[old_sy][old_sx].npcs.erase(id);
        }
        {
            lock_guard<mutex> lock(g_sectors[new_sy][new_sx].m_lock);
            if (is_player) g_sectors[new_sy][new_sx].players.insert(id);
            else g_sectors[new_sy][new_sx].npcs.insert(id);
        }
    }
}

void RemoveObjectFromSector(int id, short x, short y, bool is_player) {
    if (x == -1 || y == -1) return;
    int sx = max(0, min(NUM_SECTORS_X - 1, x / SECTOR_SIZE));
    int sy = max(0, min(NUM_SECTORS_Y - 1, y / SECTOR_SIZE));
    lock_guard<mutex> lock(g_sectors[sy][sx].m_lock);
    if (is_player) g_sectors[sy][sx].players.erase(id);
    else g_sectors[sy][sx].npcs.erase(id);
}

// --- 시야 체크 및 패킷 전송 유틸리티 ---

// 두 좌표 사이의 거리가 시야 범위(GAME_VIEW_RANGE) 내인지 확인
bool IsInView(short x1, short y1, short x2, short y2) {
    return (abs(x1 - x2) <= GAME_VIEW_RANGE && abs(y1 - y2) <= GAME_VIEW_RANGE);
}

// 특정 섹터 내의 모든 플레이어에게 패킷 전송
// 락 안에서는 세션 포인터만 수집하고, 락 밖에서 do_send 호출 (Send-Lockless 패턴)
void SendToSector(int sx, int sy, int packet_size, void* packet) {
    if (sx < 0 || sx >= NUM_SECTORS_X || sy < 0 || sy >= NUM_SECTORS_Y) return;

    vector<int> ids;
    {
        lock_guard<mutex> lock(g_sectors[sy][sx].m_lock);
        ids.reserve(g_sectors[sy][sx].players.size());
        for (int pid : g_sectors[sy][sx].players) ids.push_back(pid);
    }
    vector<shared_ptr<Player>> targets;
    targets.reserve(ids.size());
    for (int pid : ids) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, pid)) targets.push_back(a->second);
    }
    for (auto& s : targets) {
        s->do_send(packet_size, packet);
    }
}

// 주변 9개 섹터의 플레이어들에게 패킷 전송
void BroadcastToNeighbors(short x, short y, int packet_size, void* packet) {
    int sx = max(0, min(NUM_SECTORS_X - 1, x / SECTOR_SIZE));
    int sy = max(0, min(NUM_SECTORS_Y - 1, y / SECTOR_SIZE));

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            SendToSector(sx + dx, sy + dy, packet_size, packet);
        }
    }
}

// 특정 오브젝트 정보를 플레이어에게 전송 (Add)
void SendAddObject(shared_ptr<Player> to_session, shared_ptr<Player> obj_session) {
    S2C_AddObject pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_ADD_OBJECT;
    pkt.object_id = obj_session->id;
    pkt.x = obj_session->x;
    pkt.y = obj_session->y;
    pkt.visual_id = 0;
    strcpy_s(pkt.obj_name, (*atomic_load(&obj_session->name)).c_str());
    // 나머지 상태값 초기화
    pkt.hp = 100; pkt.max_hp = 100; pkt.level = 1; pkt.exp = 0;
    
    to_session->do_send(pkt.size, &pkt);
}

// 특정 오브젝트 제거 정보를 플레이어에게 전송 (Remove)
void SendRemoveObject(shared_ptr<Player> to_session, int obj_id) {
    S2C_RemoveObject pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_REMOVE_OBJECT;
    pkt.object_id = obj_id;
    
    to_session->do_send(pkt.size, &pkt);
}

// --- 함수 선언 ---
void worker_thread();
void process_packet(int client_id, unsigned char* ptr);
int RunTimerTest();

int main(int argc, char* argv[]) {
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);

    // 검증 모드: --test-timer 인자 있으면 IOCP/네트워크 초기화 없이 TimerManager만 테스트하고 종료
    if (argc > 1 && string(argv[1]) == "--test-timer") {
        return RunTimerTest();
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    g_h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    g_listen_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(g_listen_socket, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cout << "[Error] Bind Failed." << endl;
        return 1;
    }
    listen(g_listen_socket, SOMAXCONN);

    GUID guid_accept_ex = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    WSAIoctl(g_listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid_accept_ex, sizeof(guid_accept_ex), &g_fp_accept_ex, sizeof(g_fp_accept_ex), &bytes, NULL, NULL);

    CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_listen_socket), g_h_iocp, static_cast<ULONG_PTR>(-1), 0);

    cout << "[Server] MMO Server Started. Port: " << PORT << endl;

    OVERLAPPED_EX accept_over(IO_ACCEPT);
    accept_over.client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    g_fp_accept_ex(g_listen_socket, accept_over.client_socket, accept_over.buffer, 0,
        sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, NULL, &accept_over.overlapped);

    // 타이머 매니저 시작: 만기 시 IOCP에 IO_TIMER 이벤트 post
    g_timer_manager.Start([](const TimerEvent& ev) {
        auto* tov = g_timer_pool.Acquire();
        tov->type = IO_TIMER;
        tov->kind = ev.kind;
        memset(&tov->overlapped, 0, sizeof(tov->overlapped));
        PostQueuedCompletionStatus(g_h_iocp, 0,
            static_cast<ULONG_PTR>(ev.entity_id), &tov->overlapped);
    });

    vector<thread> worker_threads;
    int num_threads = thread::hardware_concurrency();
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back(worker_thread);
    }

    for (auto& t : worker_threads) t.join();
    
    closesocket(g_listen_socket);
    WSACleanup();
    return 0;
}

void worker_thread() {
    while (true) {
        DWORD bytes_transferred;
        ULONG_PTR completion_key;
        WSAOVERLAPPED* overlapped = nullptr;

        BOOL result = GetQueuedCompletionStatus(g_h_iocp, &bytes_transferred, &completion_key, (LPOVERLAPPED*)&overlapped, INFINITE);

        if (overlapped == nullptr) break;

        OVERLAPPED_EX* ov_ex = reinterpret_cast<OVERLAPPED_EX*>(overlapped);

        if (ov_ex->type == IO_ACCEPT) {
            if (g_clients.size() >= MAX_PLAYERS) {
                cout << "[Reject] Server Full." << endl;
                closesocket(ov_ex->client_socket);
            }
            else {
                SOCKET c_socket = ov_ex->client_socket;
                int new_id = g_next_id++;
                auto session = make_shared<Player>();
                session->id = new_id;
                session->socket = c_socket;
                session->is_active = true;

                {
                    ClientMap::accessor a;
                    g_clients.insert(a, new_id);
                    a->second = session;
                }
                CreateIoCompletionPort(reinterpret_cast<HANDLE>(c_socket), g_h_iocp, new_id, 0);

                cout << "[Connect] Client Connected. ID: " << new_id << " (Total: " << g_clients.size() << ")" << endl;

                S2C_LoginResult res;
                res.size = sizeof(res);
                res.type = S2C_LOGIN_RESULT;
                res.success = true;
                strcpy_s(res.message, "Connected to MMO Server!");
                session->do_send(res.size, &res);

                session->do_recv();
            }
            
            ov_ex->client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            memset(&ov_ex->overlapped, 0, sizeof(ov_ex->overlapped));
            g_fp_accept_ex(g_listen_socket, ov_ex->client_socket, ov_ex->buffer, 0,
                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, NULL, &ov_ex->overlapped);
            continue;
        }

        // 타이머 만기 이벤트는 PostQueuedCompletionStatus(bytes=0)로 들어오므로 disconnect 체크 전에 처리
        if (ov_ex->type == IO_TIMER) {
            auto* tov = reinterpret_cast<TimerOverlapped*>(ov_ex);
            // Stage 4에서 kind/entity_id 기반 NPC AI 디스패치 추가 예정. 현재는 풀 반환만.
            g_timer_pool.Release(tov);
            continue;
        }

        int client_id = static_cast<int>(completion_key);

        if (!result || bytes_transferred == 0) {
            shared_ptr<Player> disconnected;
            {
                ClientMap::const_accessor a;
                if (g_clients.find(a, client_id)) disconnected = a->second;
            }
            if (disconnected) {
                cout << "[Disconnect] Client Disconnected. ID: " << client_id << endl;
                RemoveObjectFromSector(client_id, disconnected->x, disconnected->y, true);

                // view_list 스냅샷 + 클리어 → 시야에 있던 다른 entity들에게 Remove 통보, 그쪽 view_list에서도 제거
                vector<int> viewers;
                {
                    lock_guard<mutex> lock(disconnected->view_lock);
                    viewers.assign(disconnected->view_list.begin(), disconnected->view_list.end());
                    disconnected->view_list.clear();
                }
                vector<shared_ptr<Player>> viewer_sessions;
                viewer_sessions.reserve(viewers.size());
                for (int id : viewers) {
                    ClientMap::const_accessor a;
                    if (g_clients.find(a, id)) viewer_sessions.push_back(a->second);
                }
                for (auto& other : viewer_sessions) {
                    {
                        lock_guard<mutex> lk(other->view_lock);
                        other->view_list.erase(client_id);
                    }
                    SendRemoveObject(other, client_id);
                }
            }
            g_clients.erase(client_id);
            continue;
        }

        if (ov_ex->type == IO_RECV) {
            shared_ptr<Player> session;
            {
                ClientMap::const_accessor a;
                if (g_clients.find(a, client_id)) session = a->second;
            }
            if (session) {
                
                // 1. 수신된 데이터를 링 버퍼에 기록
                if (!session->packet_buffer.Write(ov_ex->buffer, bytes_transferred)) {
                    cout << "[Error] RingBuffer overflow for Client " << client_id << endl;
                    closesocket(session->socket);
                    continue;
                }

                // 2. 완전한 패킷이 있는지 확인하고 조립 (TCP 스트림 파편화/뭉침 완전 해결)
                while (true) {
                    unsigned char packet_size = 0;
                    // 패킷의 크기 정보(첫 번째 바이트)를 엿봄
                    if (!session->packet_buffer.Peek(&packet_size, 1)) {
                        break; // 처리할 데이터가 없음
                    }

                    // 버퍼에 저장된 데이터가 패킷의 크기 이상이면 온전한 패킷 완성
                    if (session->packet_buffer.GetStoredSize() >= packet_size) {
                        unsigned char packet_data[256]; // 임시 조립 버퍼 (프로토콜상 최대 255바이트)
                        session->packet_buffer.Read(packet_data, packet_size);
                        
                        process_packet(client_id, packet_data);
                    } else {
                        // 아직 패킷이 덜 옴. 다음 Recv를 기다림
                        break; 
                    }
                }

                // 3. 다음 수신 예약
                session->do_recv();
            }
        }
        else if (ov_ex->type == IO_SEND) {
            g_send_pool.Release(ov_ex);
        }
    }
}

void process_packet(int client_id, unsigned char* ptr) {
    PACKET_TYPE type = static_cast<PACKET_TYPE>(ptr[1]);

    shared_ptr<Player> session;
    {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, client_id)) return;
        session = a->second;
    }

    switch (type) {
    case C2S_LOGIN: {
        C2S_Login* pkt = reinterpret_cast<C2S_Login*>(ptr);
        atomic_store(&session->name, make_shared<string>(pkt->username));
        
        // Spawn 위치 지정 및 Sector 등록
        session->x = rand() % WORLD_WIDTH;
        session->y = rand() % WORLD_HEIGHT;
        UpdateObjectSector(client_id, -1, -1, session->x, session->y, true);

        // 1. 본인에게 아바타 정보 전송
        S2C_AvatarInfo info;
        info.size = sizeof(info);
        info.type = S2C_AVATAR_INFO;
        info.playerId = client_id;
        info.x = session->x;
        info.y = session->y;
        info.hp = 100; info.max_hp = 100; info.exp = 0; info.level = 1; info.visualId = 0;
        session->do_send(info.size, &info);

        // 2. 초기 view_list 구축 + 상호 가시화
        int sx = session->x / SECTOR_SIZE;
        int sy = session->y / SECTOR_SIZE;
        vector<int> candidate_ids;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = sx + dx, ny = sy + dy;
                if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
                lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
                for (int other_id : g_sectors[ny][nx].players) {
                    if (other_id == client_id) continue;
                    candidate_ids.push_back(other_id);
                }
            }
        }
        vector<shared_ptr<Player>> candidates;
        candidates.reserve(candidate_ids.size());
        for (int id : candidate_ids) {
            ClientMap::const_accessor a;
            if (g_clients.find(a, id)) candidates.push_back(a->second);
        }
        vector<shared_ptr<Player>> in_view;
        {
            lock_guard<mutex> lock(session->view_lock);
            for (auto& other : candidates) {
                if (IsInView(session->x, session->y, other->x, other->y)) {
                    session->view_list.insert(other->id);
                    in_view.push_back(other);
                }
            }
        }
        for (auto& other : in_view) {
            {
                lock_guard<mutex> lock(other->view_lock);
                other->view_list.insert(client_id);
            }
            SendAddObject(other, session); // 타인에게 나를 추가
            SendAddObject(session, other); // 나에게 타인을 추가
        }

        cout << "[Login] Client " << client_id << " logged in as: " << *atomic_load(&session->name) << " at (" << session->x << ", " << session->y << ")" << endl;
        break;
    }
    case C2S_MOVE: {
        C2S_Move* pkt = reinterpret_cast<C2S_Move*>(ptr);
        short old_x = session->x;
        short old_y = session->y;
        
        // Sector 업데이트
        UpdateObjectSector(client_id, old_x, old_y, pkt->x, pkt->y, true);
        session->x = pkt->x;
        session->y = pkt->y;

        // 이동 패킷 브로드캐스팅
        S2C_MoveObject move_pkt;
        move_pkt.size = sizeof(move_pkt);
        move_pkt.type = S2C_MOVE_OBJECT;
        move_pkt.object_id = client_id;
        move_pkt.x = session->x;
        move_pkt.y = session->y;
        move_pkt.move_time = pkt->move_time;

        // view_list 기반 차분: 새 위치 9섹터에서 후보 수집 → 시야 필터 → 기존 view_list와 diff
        // (기존 9섹터 풀스캔 방식은 멀리 이동 시 old view에 있던 원거리 entity에게 Remove를 못 보내는 버그가 있었음)
        int new_sx = session->x / SECTOR_SIZE;
        int new_sy = session->y / SECTOR_SIZE;

        vector<int> candidate_ids;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = new_sx + dx, ny = new_sy + dy;
                if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
                lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
                for (int other_id : g_sectors[ny][nx].players) {
                    if (other_id == client_id) continue;
                    candidate_ids.push_back(other_id);
                }
            }
        }

        // 후보 + 기존 view_list의 모든 ID를 일괄 조회
        unordered_set<int> all_ids(candidate_ids.begin(), candidate_ids.end());
        {
            lock_guard<mutex> lock(session->view_lock);
            for (int id : session->view_list) all_ids.insert(id);
        }
        unordered_map<int, shared_ptr<Player>> id_to_session;
        for (int id : all_ids) {
            ClientMap::const_accessor a;
            if (g_clients.find(a, id)) id_to_session[id] = a->second;
        }

        // 시야 안인 entity 집합 (새 view)
        unordered_set<int> new_view;
        for (int id : candidate_ids) {
            auto it = id_to_session.find(id);
            if (it == id_to_session.end()) continue;
            if (IsInView(session->x, session->y, it->second->x, it->second->y)) {
                new_view.insert(id);
            }
        }

        // diff: entered = new - old, left = old - new, stayed = new ∩ old. 한 락 안에서 처리 후 교체
        vector<int> entered_ids, left_ids, stayed_ids;
        {
            lock_guard<mutex> lock(session->view_lock);
            for (int id : new_view) {
                if (session->view_list.count(id)) stayed_ids.push_back(id);
                else entered_ids.push_back(id);
            }
            for (int id : session->view_list) {
                if (!new_view.count(id)) left_ids.push_back(id);
            }
            session->view_list = new_view;
        }

        // 새로 시야에 들어온 entity: 상호 view_list 업데이트 + Add 전송
        for (int id : entered_ids) {
            auto it = id_to_session.find(id);
            if (it == id_to_session.end()) continue;
            auto& other = it->second;
            {
                lock_guard<mutex> lk(other->view_lock);
                other->view_list.insert(client_id);
            }
            SendAddObject(other, session);
            SendAddObject(session, other);
        }
        // 시야에서 벗어난 entity: 상호 view_list 정리 + Remove 전송
        for (int id : left_ids) {
            auto it = id_to_session.find(id);
            if (it == id_to_session.end()) continue;
            auto& other = it->second;
            {
                lock_guard<mutex> lk(other->view_lock);
                other->view_list.erase(client_id);
            }
            SendRemoveObject(other, client_id);
            SendRemoveObject(session, id);
        }
        // 유지된 entity: Move 패킷 전송
        for (int id : stayed_ids) {
            auto it = id_to_session.find(id);
            if (it == id_to_session.end()) continue;
            it->second->do_send(move_pkt.size, &move_pkt);
        }
        // 본인에게도 이동 확인 패킷 (클라이언트의 자기 위치 갱신용)
        session->do_send(move_pkt.size, &move_pkt);
        break;
    }
    case C2S_LOGOUT: {
        cout << "[Logout] Client " << client_id << " requested logout." << endl;
        // closesocket → IO 실패 → disconnect 경로가 view_list 정리/Remove 전송 담당
        closesocket(session->socket);
        break;
    }
    default:
        break;
    }
}

// --- TimerManager 검증 테스트 (--test-timer 모드) ---
// N개의 더미 이벤트를 랜덤 딜레이로 schedule → 만기 순서/시간 정확도 측정 후 PASS/FAIL 출력
int RunTimerTest() {
    using namespace chrono;
    constexpr int N = 1000;
    constexpr int MIN_DELAY_MS = 50;
    constexpr int MAX_DELAY_MS = 5000;
    constexpr long long TOLERANCE_MS = 100;

    cout << "[Timer Test] Scheduling " << N << " events, delays "
         << MIN_DELAY_MS << "~" << MAX_DELAY_MS << "ms..." << endl;

    struct FireRecord { int entity_id; steady_clock::time_point fire_time; };
    vector<FireRecord> fires;
    fires.reserve(N);
    mutex log_mu;

    TimerManager tm;
    tm.Start([&](const TimerEvent& ev) {
        auto now = steady_clock::now();
        lock_guard<mutex> lock(log_mu);
        fires.push_back({ ev.entity_id, now });
    });

    vector<int> expected_delays(N);
    auto test_start = steady_clock::now();
    for (int i = 0; i < N; ++i) {
        int d = MIN_DELAY_MS + (rand() % (MAX_DELAY_MS - MIN_DELAY_MS));
        expected_delays[i] = d;
        tm.Schedule(i, TimerEventKind::TestPing, d);
    }

    this_thread::sleep_for(milliseconds(MAX_DELAY_MS + 1500));
    tm.Stop();

    const int fired = static_cast<int>(fires.size());
    bool ordered = true;
    for (size_t i = 1; i < fires.size(); ++i) {
        if (fires[i].fire_time < fires[i - 1].fire_time) {
            ordered = false;
            cout << "[Timer Test] Out of order at index " << i << endl;
            break;
        }
    }

    long long max_lag = 0, total_lag = 0;
    int over_tolerance = 0;
    for (const auto& fr : fires) {
        auto expected = test_start + milliseconds(expected_delays[fr.entity_id]);
        long long lag = duration_cast<milliseconds>(fr.fire_time - expected).count();
        if (lag < 0) lag = 0;
        if (lag > max_lag) max_lag = lag;
        if (lag > TOLERANCE_MS) over_tolerance++;
        total_lag += lag;
    }
    long long avg_lag = (fired > 0) ? (total_lag / fired) : 0;

    cout << "[Timer Test] Fired " << fired << "/" << N << endl;
    cout << "[Timer Test] Ordering: " << (ordered ? "PASS" : "FAIL") << endl;
    cout << "[Timer Test] Lag avg=" << avg_lag << "ms, max=" << max_lag
         << "ms, over " << TOLERANCE_MS << "ms tolerance: " << over_tolerance << endl;

    bool pass = (fired == N) && ordered && (over_tolerance == 0);
    cout << "[Timer Test] Overall: " << (pass ? "PASS" : "FAIL") << endl;
    return pass ? 0 : 1;
}
