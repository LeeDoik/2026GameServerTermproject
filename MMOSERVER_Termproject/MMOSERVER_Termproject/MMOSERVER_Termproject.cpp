#define NOMINMAX
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <algorithm>
#include <winsock2.h>
#include <mswsock.h>
#include <tbb/concurrent_map.h>
#include <mutex>
#include <unordered_set>
#include "protocol_2026.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

using namespace std;

// --- 구조체 및 상수 정의 ---
enum IO_TYPE { IO_RECV, IO_SEND, IO_ACCEPT };

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

struct SESSION {
    int id;
    SOCKET socket;
    OVERLAPPED_EX recv_overlapped;
    RingBuffer packet_buffer; // 링 버퍼 객체 추가
    shared_ptr<string> name; 
    short x, y;
    bool is_active;

    SESSION() : id(-1), socket(INVALID_SOCKET), is_active(false), recv_overlapped(IO_RECV) {
        name = make_shared<string>("Guest");
        x = -1;
        y = -1;
    }

    ~SESSION() {
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
        OVERLAPPED_EX* ov = new OVERLAPPED_EX(IO_SEND);
        ov->wsa_buf.len = num_bytes;
        memcpy(ov->buffer, mess, num_bytes);
        WSASend(socket, &ov->wsa_buf, 1, NULL, 0, &ov->overlapped, NULL);
    }
};

// --- 글로벌 변수 ---
tbb::concurrent_map<int, shared_ptr<SESSION>> g_clients;
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
void SendToSector(int sx, int sy, int packet_size, void* packet) {
    if (sx < 0 || sx >= NUM_SECTORS_X || sy < 0 || sy >= NUM_SECTORS_Y) return;

    lock_guard<mutex> lock(g_sectors[sy][sx].m_lock);
    for (int pid : g_sectors[sy][sx].players) {
        auto it = g_clients.find(pid);
        if (it != g_clients.end()) {
            it->second->do_send(packet_size, packet);
        }
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
void SendAddObject(shared_ptr<SESSION> to_session, shared_ptr<SESSION> obj_session) {
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
void SendRemoveObject(shared_ptr<SESSION> to_session, int obj_id) {
    S2C_RemoveObject pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_REMOVE_OBJECT;
    pkt.object_id = obj_id;
    
    to_session->do_send(pkt.size, &pkt);
}

// --- 함수 선언 ---
void worker_thread();
void process_packet(int client_id, unsigned char* ptr);

int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);

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
                auto session = make_shared<SESSION>();
                session->id = new_id;
                session->socket = c_socket;
                session->is_active = true;

                g_clients[new_id] = session;
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

        int client_id = static_cast<int>(completion_key);

        if (!result || bytes_transferred == 0) {
            auto it = g_clients.find(client_id);
            if (it != g_clients.end()) {
                cout << "[Disconnect] Client Disconnected. ID: " << client_id << endl;
                RemoveObjectFromSector(client_id, it->second->x, it->second->y, true);
                g_clients.unsafe_erase(it);
            }
            continue;
        }

        if (ov_ex->type == IO_RECV) {
            auto it = g_clients.find(client_id);
            if (it != g_clients.end()) {
                auto session = it->second;
                
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
            delete ov_ex;
        }
    }
}

void process_packet(int client_id, unsigned char* ptr) {
    PACKET_TYPE type = static_cast<PACKET_TYPE>(ptr[1]);

    auto it = g_clients.find(client_id);
    if (it == g_clients.end()) return;
    auto session = it->second;

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

        // 2. 주변 플레이어들에게 나를 알리고, 나에게 주변 플레이어들을 알림
        int sx = session->x / SECTOR_SIZE;
        int sy = session->y / SECTOR_SIZE;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = sx + dx, ny = sy + dy;
                if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
                
                lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
                for (int other_id : g_sectors[ny][nx].players) {
                    if (other_id == client_id) continue;
                    auto other_it = g_clients.find(other_id);
                    if (other_it != g_clients.end()) {
                        auto other_session = other_it->second;
                        if (IsInView(session->x, session->y, other_session->x, other_session->y)) {
                            SendAddObject(other_session, session); // 타인에게 나를 추가
                            SendAddObject(session, other_session); // 나에게 타인을 추가
                        }
                    }
                }
            }
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

        // 주변 섹터에 알림 (시야 경계 처리 포함)
        int old_sx = old_x / SECTOR_SIZE;
        int old_sy = old_y / SECTOR_SIZE;
        int new_sx = session->x / SECTOR_SIZE;
        int new_sy = session->y / SECTOR_SIZE;

        // 단순히 주변에 이동 알림 (최적화 전: 주변 9개 섹터 브로드캐스트)
        // 실제로는 새로 시야에 들어온 사람에게는 Add, 나간 사람에게는 Remove를 보내야 함
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = new_sx + dx, ny = new_sy + dy;
                if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
                
                lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
                for (int other_id : g_sectors[ny][nx].players) {
                    auto other_it = g_clients.find(other_id);
                    if (other_it == g_clients.end()) continue;
                    auto other_session = other_it->second;

                    bool was_in_view = IsInView(old_x, old_y, other_session->x, other_session->y);
                    bool is_in_view = IsInView(session->x, session->y, other_session->x, other_session->y);

                    if (is_in_view) {
                        if (was_in_view) {
                            other_session->do_send(move_pkt.size, &move_pkt);
                        } else {
                            // 새로 시야에 들어옴
                            SendAddObject(other_session, session);
                            SendAddObject(session, other_session);
                        }
                    } else if (was_in_view) {
                        // 시야에서 벗어남
                        SendRemoveObject(other_session, client_id);
                        SendRemoveObject(session, other_id);
                    }
                }
            }
        }
        break;
    }
    case C2S_LOGOUT: {
        cout << "[Logout] Client " << client_id << " requested logout." << endl;
        
        // 주변 플레이어들에게 나를 제거하라고 알림
        S2C_RemoveObject rem_pkt;
        rem_pkt.size = sizeof(rem_pkt);
        rem_pkt.type = S2C_REMOVE_OBJECT;
        rem_pkt.object_id = client_id;
        BroadcastToNeighbors(session->x, session->y, rem_pkt.size, &rem_pkt);

        closesocket(session->socket);
        break;
    }
    default:
        break;
    }
}
