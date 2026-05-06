#define NOMINMAX
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <winsock2.h>
#include <mswsock.h>
#include <tbb/concurrent_map.h>
#include "protocol_2026.h"

#pragma comment(lib, "ws2_32.lib")

using namespace std;

// --- 구조체 및 상수 정의 ---
enum class IO_TYPE { RECV, SEND, ACCEPT };

struct OVERLAPPED_EX {
    OVERLAPPED overlapped;
    IO_TYPE type;
    WSABUF wsa_buf;
    unsigned char buffer[MAX_CHAT_MSG_LEN + 256]; // 여유 있는 버퍼 크기
};

struct SESSION {
    int id;
    SOCKET socket;
    OVERLAPPED_EX recv_overlapped;
    shared_ptr<string> name; // atomic<shared_ptr> 호환성 문제로 shared_ptr로 변경 후 필요시 mutex/atomic_load 사용
    short x, y;
    bool is_active;

    SESSION() : id(-1), socket(INVALID_SOCKET), is_active(false) {
        name = make_shared<string>("Guest");
        x = y = 0;
        memset(&recv_overlapped.overlapped, 0, sizeof(recv_overlapped.overlapped));
        recv_overlapped.type = IO_TYPE::RECV;
        recv_overlapped.wsa_buf.buf = reinterpret_cast<char*>(recv_overlapped.buffer);
        recv_overlapped.wsa_buf.len = sizeof(recv_overlapped.buffer);
    }

    ~SESSION() {
        if (socket != INVALID_SOCKET) closesocket(socket);
    }
};

// --- 글로벌 변수 ---
tbb::concurrent_map<int, shared_ptr<SESSION>> g_clients;
atomic<int> g_next_id{ 0 };
HANDLE g_h_iocp;

// --- 함수 선언 ---
void worker_thread();
void do_recv(shared_ptr<SESSION>& session);
void process_packet(int client_id, unsigned char* ptr);

int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    g_h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    SOCKET listen_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listen_socket, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cout << "[Error] Bind Failed." << endl;
        return 1;
    }
    listen(listen_socket, SOMAXCONN);

    cout << "[Server] MMO Server Started. Listening on port " << PORT << "..." << endl;

    vector<thread> worker_threads;
    int num_threads = thread::hardware_concurrency();
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back(worker_thread);
    }

    while (true) {
        SOCKADDR_IN client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client_socket = accept(listen_socket, (SOCKADDR*)&client_addr, &addr_len);

        if (client_socket == INVALID_SOCKET) continue;

        int new_id = g_next_id++;
        auto session = make_shared<SESSION>();
        session->id = new_id;
        session->socket = client_socket;
        session->is_active = true;

        g_clients[new_id] = session;

        CreateIoCompletionPort(reinterpret_cast<HANDLE>(client_socket), g_h_iocp, new_id, 0);

        cout << "[Connect] Client Connected. ID: " << new_id << " (Total: " << g_clients.size() << ")" << endl;

        do_recv(session);
    }

    for (auto& t : worker_threads) t.join();
    closesocket(listen_socket);
    WSACleanup();
    return 0;
}

void do_recv(shared_ptr<SESSION>& session) {
    DWORD flags = 0;
    DWORD recv_bytes = 0;
    memset(&session->recv_overlapped.overlapped, 0, sizeof(session->recv_overlapped.overlapped));
    WSARecv(session->socket, &session->recv_overlapped.wsa_buf, 1, &recv_bytes, &flags, &session->recv_overlapped.overlapped, NULL);
}

void worker_thread() {
    while (true) {
        DWORD bytes_transferred;
        ULONG_PTR completion_key;
        OVERLAPPED* overlapped = nullptr;

        BOOL result = GetQueuedCompletionStatus(g_h_iocp, &bytes_transferred, &completion_key, &overlapped, INFINITE);

        if (overlapped == nullptr) break;

        int client_id = static_cast<int>(completion_key);
        OVERLAPPED_EX* ov_ex = reinterpret_cast<OVERLAPPED_EX*>(overlapped);

        if (!result || bytes_transferred == 0) {
            // 접속 종료 처리
            auto it = g_clients.find(client_id);
            if (it != g_clients.end()) {
                cout << "[Disconnect] Client Disconnected. ID: " << client_id << endl;
                g_clients.unsafe_erase(it);
            }
            continue;
        }

        if (ov_ex->type == IO_TYPE::RECV) {
            process_packet(client_id, ov_ex->buffer);
            auto it = g_clients.find(client_id);
            if (it != g_clients.end()) {
                do_recv(it->second);
            }
        }
    }
}

void process_packet(int client_id, unsigned char* ptr) {
    unsigned char size = ptr[0];
    PACKET_TYPE type = static_cast<PACKET_TYPE>(ptr[1]);

    auto it = g_clients.find(client_id);
    if (it == g_clients.end()) return;
    auto session = it->second;

    switch (type) {
    case C2S_LOGIN: {
        C2S_Login* pkt = reinterpret_cast<C2S_Login*>(ptr);
        // C++20 atomic shared_ptr 대용 (atomic_store 호환성)
        atomic_store(&session->name, make_shared<string>(pkt->username));
        
        cout << "[Login] Client " << client_id << " logged in as: " << *atomic_load(&session->name) << endl;

        S2C_LoginResult res;
        res.size = sizeof(res);
        res.type = S2C_LOGIN_RESULT;
        res.success = true;
        strcpy_s(res.message, "Welcome to MMO Server 2026!");
        
        send(session->socket, reinterpret_cast<char*>(&res), res.size, 0);
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
