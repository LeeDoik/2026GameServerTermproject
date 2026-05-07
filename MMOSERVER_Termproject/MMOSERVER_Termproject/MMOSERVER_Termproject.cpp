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
#include "protocol_2026.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

using namespace std;

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
    int GetFreeSpace() const { return capacity - current_size; }

    bool Write(const unsigned char* data, int size) {
        if (size > GetFreeSpace()) return false; // 오버플로우 방지
        
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

// --- 구조체 및 상수 정의 ---
enum IO_TYPE { IO_RECV, IO_SEND, IO_ACCEPT };

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
        x = y = 0;
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
        
        cout << "[Login] Client " << client_id << " logged in as: " << *atomic_load(&session->name) << endl;
        break;
    }
    case C2S_LOGOUT: {
        cout << "[Logout] Client " << client_id << " requested logout." << endl;
        closesocket(session->socket);
        break;
    }
    default:
        break;
    }
}
