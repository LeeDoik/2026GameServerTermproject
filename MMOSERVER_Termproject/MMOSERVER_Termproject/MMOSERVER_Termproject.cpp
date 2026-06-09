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
#include <random>
#include <chrono>
#include <fstream>
#include "protocol_2026.h"
#include "Core/ObjectPool.h"
#include "Core/Entity.h"
#include "Core/OverlappedTypes.h"
#include "Core/TimerManager.h"
#include "Core/NPC.h"
#include "Core/World.h"
#include "Core/NpcSpawner.h"
#include "Core/LuaVM.h"
#include "Core/GameConfig.h"
#include "Core/Map.h"
#include "Core/AStar.h"
#include "Core/Item.h"
#include "Core/Quest.h"
#include "Core/Db/DbTypes.h"
#include "Core/Db/JsonFileBackend.h"
#include "Core/Db/SqlServerOdbcBackend.h"
#include "Core/Db/DbWorker.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

// 핫패스 로깅 토글 — 1400 CCU 한계는 stdout 직렬화가 주 원인.
// 디버그/시연 시 1로 변경하면 매 connect/login/disconnect/logout 콘솔 출력.
// 운영/부하 테스트 시에는 0 권장 (5000 CCU 목표).
#define VERBOSE_CLIENT_EVENTS 0

using namespace std;

// 링 버퍼 클래스 전방 선언
class RingBuffer;

// [perf 2A] OVERLAPPED_EX 물리 버퍼 크기. 송신 코얼레싱 1회 WSASend의 적재 상한.
// recv와 공유하지만 do_recv는 wsa_buf.len을 RECV_BUF_LEN으로 캡해 RingBuffer(8192) 안전을 유지한다.
constexpr int IO_BUF_SIZE   = 8192;
constexpr int RECV_BUF_LEN  = MAX_CHAT_MSG_LEN + 256;  // recv 1회 읽기 상한 (기존 동작 보존)
constexpr int SEND_QUEUE_CAP = 256 * 1024;             // 세션 송신 큐 상한 — 초과 시 느린 소비자로 보고 연결 종료

struct OVERLAPPED_EX {
    WSAOVERLAPPED overlapped;
    IO_TYPE type;
    WSABUF wsa_buf;
    SOCKET client_socket;
    unsigned char pool_shard;   // [perf] IO_SEND이 어느 송신 풀 샤드에서 왔는지 (Release 라우팅용)
    unsigned char buffer[IO_BUF_SIZE];

    OVERLAPPED_EX() {
        memset(&overlapped, 0, sizeof(overlapped));
        type = IO_RECV;
        wsa_buf.buf = reinterpret_cast<char*>(buffer);
        wsa_buf.len = sizeof(buffer);
        client_socket = INVALID_SOCKET;
        pool_shard = 0;
    }
    OVERLAPPED_EX(IO_TYPE t) : OVERLAPPED_EX() { type = t; }
};

// IO_SEND 전용 OVERLAPPED_EX 풀. do_send마다 new/delete 비용 제거.
// [perf] 단일 풀의 전역 락은 모든 do_send/완료가 다투던 병목이었다 → 샤드 배열로 분산.
// Acquire는 워커별 고정 샤드를 쓰고(락 경합 분산), Release는 객체에 박힌 pool_shard로
// 원래 샤드에 정확히 반환한다(IO_SEND 완료가 다른 워커에서 일어날 수 있으므로).
constexpr int SEND_POOL_SHARDS = 8;
ObjectPool<OVERLAPPED_EX> g_send_pools[SEND_POOL_SHARDS];

// 호출 스레드에 고정된 송신 풀 샤드 인덱스 (첫 사용 시 라운드로빈 배정).
static int WorkerSendShard() {
    thread_local int idx = -1;
    if (idx < 0) {
        static std::atomic<int> next{ 0 };
        idx = next.fetch_add(1) % SEND_POOL_SHARDS;
    }
    return idx;
}

// Timer 만기 이벤트를 IOCP로 post할 때 사용하는 OVERLAPPED 풀
ObjectPool<TimerOverlapped> g_timer_pool;

// 전역 타이머 매니저. main에서 Start, worker_thread가 IO_TIMER로 처리
TimerManager g_timer_manager;

// 전역 Lua VM. 부팅 시 npc_ai.lua 로드 검증 + 로그용. AI 핫패스는 워커별 VM 사용.
LuaVM g_lua;

// [perf] 워커 로컬 Lua 인터프리터 인프라.
// 단일 g_lua + 단일 mutex로 모든 NPC AI 틱을 직렬화하던 병목을 제거하기 위해,
// 각 워커 스레드가 자기 lua_State를 갖는다(thread_local). OnTick/OnBossTick은
// stateless(모든 상태를 ctx로 전달)이므로 어느 VM에서 실행해도 결과가 동일하다.
// 인터프리터 개수 = 워커 스레드 수(코어 수)이며 NPC 수와 무관하다.
std::string g_lua_script_path;  // 부팅 시 main()이 해석. 워커 VM이 동일 경로 재사용.

// 모든 워커/부팅 VM에 동일한 스펙 상수를 노출 + 워커별 고유 RNG 시드 주입.
static void SetupLuaConstants(LuaVM& vm) {
    vm.SetGlobalInt("TYPE_PEACE",        static_cast<long long>(NpcType::Peace));
    vm.SetGlobalInt("TYPE_AGRO",         static_cast<long long>(NpcType::Agro));
    vm.SetGlobalInt("TYPE_BOSS",         static_cast<long long>(NpcType::Boss));
    vm.SetGlobalInt("MOVE_FIXED",        static_cast<long long>(NpcMoveMode::Fixed));
    vm.SetGlobalInt("MOVE_ROAMING",      static_cast<long long>(NpcMoveMode::Roaming));
    vm.SetGlobalInt("AGRO_RANGE",        AGRO_DETECT_RANGE);
    vm.SetGlobalInt("ROAM_RANGE",        ROAM_AREA_RANGE);
    vm.SetGlobalInt("BOSS_AGRO_RANGE",   BOSS_AGRO_RANGE);
    vm.SetGlobalInt("BOSS_AOE_INTERVAL", BOSS_AOE_INTERVAL_TICKS);
    vm.SetGlobalInt("BOSS_CHAT_INTERVAL",BOSS_CHAT_INTERVAL_TICKS);
    // 워커별 고유 시드 — 모든 워커가 동일 로밍 난수 시퀀스를 도는 것을 방지.
    // npc_ai.lua는 math.randomseed(WORKER_SEED or os.time())로 이 값을 사용.
    long long seed = static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())
        ^ static_cast<long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    vm.SetGlobalInt("WORKER_SEED", seed);
}

// VM 1개를 AI 실행 가능 상태로 초기화. 반환값 = npc_ai.lua 로드 성공 여부.
static bool InitWorkerLuaVM(LuaVM& vm) {
    vm.OpenStdLibs();
    SetupLuaConstants(vm);
    if (g_lua_script_path.empty()) return false;
    return vm.DoFile(g_lua_script_path.c_str());
}

// 현재 워커 스레드 전용 Lua VM. 첫 호출 시 lazy init.
static LuaVM& GetWorkerLua() {
    thread_local LuaVM t_lua;
    thread_local bool t_ready = false;
    if (!t_ready) {
        InitWorkerLuaVM(t_lua);
        t_ready = true;
    }
    return t_lua;
}

// Stage 6.3: DB 워커. JSON 파일 백엔드를 기본으로 시작.
DbWorker g_db_worker;

// IOCP에 DB 완료 통보용 OVERLAPPED. 첫 두 필드(WSAOVERLAPPED, IO_TYPE) 레이아웃 호환.
struct DbOverlapped {
    WSAOVERLAPPED overlapped;
    IO_TYPE type;
    DbResponse response;

    DbOverlapped() : type(IO_DB_DONE) {
        memset(&overlapped, 0, sizeof(overlapped));
    }
};
ObjectPool<DbOverlapped> g_db_pool;

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
    // [perf 2A] 세션별 송신 코얼레싱 — send_lock가 send_queue/send_in_flight 보호.
    // 한 틱에 쌓인 여러 S2C 패킷을 1회 WSASend로 합치고, in-flight 1건으로 소켓당 송신 순서 보장.
    std::mutex send_lock;
    std::vector<unsigned char> send_queue;
    bool send_in_flight = false;
    shared_ptr<string> name;
    bool is_active;

    // 위치(x/y) + 섹터 갱신의 임계영역 보호. 플레이어 좌표는 본인 패킷 스레드(HandleMove)와
    // NPC 틱 스레드(PlayerOnDeath의 리스폰 텔레포트)가 동시에 바꿀 수 있다. 두 경로가 각각
    // "현재 좌표 읽기 → 섹터 이동 → 좌표 쓰기"를 하는데, 이 락 없이는 서로 끼어들어
    // 섹터 소속이 깨지거나(유령/미표시), 사망 직후 stale 이동이 좌표를 덮어써 클라(스폰)와
    // 서버(이전 칸)가 어긋나 이후 모든 이동이 >1칸으로 거부되는 '리스폰 후 멈춤'이 발생한다.
    // move_lock은 항상 sector/view 락보다 먼저(최외곽) 잡아 데드락을 피한다.
    std::mutex move_lock;

    // 완전 입장(OnPlayerSpawn 종료) 여부. name은 HandleLogin에서 DB Load 큐잉 '전'에 세팅되므로
    // name만으로 게이팅하면 (로그인~스폰) 사이 윈도우에 들어온 이동/공격이 좌표 미설정(-1,-1)
    // 세션을 건드려 섹터가 깨질 수 있다. 스폰 완료 후에만 게임플레이 패킷을 처리한다.
    std::atomic<bool> spawned{ false };

    // 전투/스탯 — Stage 5
    atomic<int> hp;
    atomic<int> max_hp;
    atomic<int> mp;       // MP 시스템 (영속 안 함 — 로그인 시 max로 시작)
    atomic<int> max_mp;
    atomic<unsigned long long> exp;
    atomic<unsigned char> level;
    atomic<long long> last_attack_ms;  // 쿨타임 검증용

    // 스킬 쿨타임 — Stage 7
    atomic<long long> last_skill1_ms;  // AoE 쿨타임
    atomic<long long> last_skill2_ms;  // Line 쿨타임
    atomic<long long> last_skill3_ms;  // Heal 쿨타임

    // 파티 — Stage 7 파티
    atomic<int> party_id{ -1 };  // -1 = 파티 없음

    // 아이템 — Stage 8
    mutex inv_lock;                              // inventory 변경 보호
    vector<pair<int, int>> inventory;            // (item_id, qty), inv_lock 보호
    atomic<int> equipped_weapon_id{ -1 };        // 장착 무기 item_id (-1=없음)
    atomic<int> equipped_armor_id{ -1 };         // 장착 방어구 item_id (-1=없음). max_hp에 보너스 직접 합산
    atomic<int> atk_bonus{ 0 };                  // 장착 무기 공격력 보너스 (데미지 핫패스 lockless 읽기)

    // 퀘스트 — Stage 9
    struct QuestProgress { int quest_id; int kill_count; unsigned char state; }; // state 0=active,1=completed
    mutex quest_lock;                            // quests 변경 보호
    vector<QuestProgress> quests;                // 진행중 + 완료 모두 보관, quest_lock 보호

    Player() : Entity(), socket(INVALID_SOCKET), recv_overlapped(IO_RECV), is_active(false),
               hp(100), max_hp(100), mp(MP_MAX), max_mp(MP_MAX), exp(0), level(1), last_attack_ms(0),
               last_skill1_ms(0), last_skill2_ms(0), last_skill3_ms(0) {
        name = make_shared<string>("");
    }

    ~Player() override {
        if (socket != INVALID_SOCKET) closesocket(socket);
    }

    void do_recv() {
        DWORD flags = 0;
        DWORD recv_bytes = 0;
        memset(&recv_overlapped.overlapped, 0, sizeof(recv_overlapped.overlapped));
        recv_overlapped.wsa_buf.buf = reinterpret_cast<char*>(recv_overlapped.buffer);
        recv_overlapped.wsa_buf.len = RECV_BUF_LEN;  // 물리 버퍼는 커졌지만 recv는 기존 상한으로 캡 (RingBuffer 안전)
        WSARecv(socket, &recv_overlapped.wsa_buf, 1, &recv_bytes, &flags, &recv_overlapped.overlapped, NULL);
    }

    // [perf 2A] send_lock 보유 상태에서 호출. send_queue 앞에서 한 청크(≤IO_BUF_SIZE)를 풀 객체에
    // 담아 WSASend 1회. 완료는 worker_thread의 IO_SEND 분기가 처리(부분전송 되돌림 + 다음 flush).
    void flush_locked() {
        int shard = WorkerSendShard();
        OVERLAPPED_EX* ov = g_send_pools[shard].Acquire();
        ov->pool_shard = static_cast<unsigned char>(shard);  // Release가 원래 샤드로 반환
        ov->type = IO_SEND;
        memset(&ov->overlapped, 0, sizeof(ov->overlapped));
        int n = static_cast<int>((std::min)(send_queue.size(), static_cast<size_t>(IO_BUF_SIZE)));
        memcpy(ov->buffer, send_queue.data(), n);
        send_queue.erase(send_queue.begin(), send_queue.begin() + n);
        ov->wsa_buf.buf = reinterpret_cast<char*>(ov->buffer);
        ov->wsa_buf.len = n;
        int r = WSASend(socket, &ov->wsa_buf, 1, NULL, 0, &ov->overlapped, NULL);
        if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            // 송신 시작 실패(소켓 사망) → 풀 회수 + in-flight 해제. 단절은 IOCP recv 완료가 감지.
            g_send_pools[ov->pool_shard].Release(ov);
            send_in_flight = false;
        }
    }

    // [perf 2A] fire-and-forget → 세션 큐에 적재 후 idle이면 1회 flush. 패킷당 WSASend 폭주 제거.
    void do_send(int num_bytes, void* mess) {
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lk(send_lock);
            if (send_queue.size() + static_cast<size_t>(num_bytes) > SEND_QUEUE_CAP) {
                overflow = true;  // 느린 소비자 — 락 밖에서 종료
            } else {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(mess);
                send_queue.insert(send_queue.end(), p, p + num_bytes);
                if (!send_in_flight) {
                    send_in_flight = true;
                    flush_locked();
                }
            }
        }
        if (overflow) closesocket(socket);  // 락 밖에서: IOCP가 단절 정리
    }
};

// === 파티 시스템 ===
constexpr int MAX_PARTY_SIZE = 4;

struct Party {
    int id;
    int leader_id;
    vector<int> members;  // g_party_mutex로 보호
};

unordered_map<int, shared_ptr<Party>> g_parties;
// 펜딩 초대. PartyInviteExpire 타이머는 invitee_id만 키로 전달하므로, 같은 invitee가
// 이전 초대 해소(거절/수락/탈퇴) 후 30초 내 재초대되면 묵은 타이머가 새 초대를 잘못
// 취소할 수 있다. expire_at_ms를 들고, 만료 핸들러가 "지금이 만료시각 이전이면 더 새
// 초대가 덮어쓴 것"으로 보고 무시해 묵은 타이머로부터 새 초대를 보호한다.
struct PendingInvite {
    int inviter_id;
    long long expire_at_ms;  // steady_clock 기준 만료 시각
};
unordered_map<int, PendingInvite> g_pending_invites;  // invitee_id → PendingInvite
mutex g_party_mutex;
atomic<int> g_next_party_id{ 1 };

// === 바닥 아이템 (Stage 8) ===
struct GroundItem {
    int   item_id;
    int   count;
    short x;
    short y;
};
unordered_map<int, GroundItem> g_ground_items;  // drop_id → GroundItem
mutex g_ground_mutex;
atomic<int> g_next_drop_id{ DROP_ID_START };

// --- 글로벌 변수 ---
// TBB concurrent_hash_map: 버킷 단위 락. accessor 패턴으로 find/insert/erase 모두 동시 안전
using ClientMap = tbb::concurrent_hash_map<int, std::shared_ptr<Player>>;
ClientMap g_clients;
atomic<int> g_next_id{ 1 };  // id 0은 시스템 메시지(S2C_CHAT_MESSAGE object_id=0) 전용 sentinel — 플레이어 id는 1부터

// 이름→client_id 인덱스 (파티 초대 등 이름 탐색용).
// g_clients(tbb concurrent_hash_map)를 begin()/end()로 직접 순회하면 다른 워커의 동시 erase로
// 이터레이터가 무효화될 수 있어, O(1) 조회용 별도 인덱스를 둔다. 동명이인 대비 multimap + 전용 mutex.
// (login/disconnect/invite만 접근 — 핫패스가 아니라 락 경합 무시 가능)
std::unordered_multimap<std::string, int> g_name_index;
std::mutex g_name_index_mutex;
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
    pkt.hp     = obj_session->hp.load();
    pkt.max_hp = obj_session->max_hp.load();
    pkt.level  = obj_session->level.load();
    pkt.exp    = obj_session->exp.load();
    
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

// --- NPC 관련 헬퍼 (Stage 4) ---

// NPC 정보를 S2C_AddObject 포맷으로 플레이어에게 전송. client는 object_id >= NPC_ID_START로 NPC 식별
void SendAddNpc(shared_ptr<Player> to_session, NPC& npc) {
    S2C_AddObject pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_ADD_OBJECT;
    pkt.object_id = npc.id;
    pkt.x = npc.x;
    pkt.y = npc.y;
    pkt.visual_id = npc.visual_id;
    strncpy_s(pkt.obj_name, sizeof(pkt.obj_name), npc.name, _TRUNCATE);
    pkt.hp = npc.hp;
    pkt.max_hp = npc.max_hp;
    pkt.level = npc.level;
    pkt.exp = 0;
    to_session->do_send(pkt.size, &pkt);
}

// 워커 스레드 로컬 RNG. NPC AI tick에서 1칸 이동 방향 결정
static thread_local std::mt19937 t_npc_rng{
    static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        ^ static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))
};

void NpcOnMove(int npc_id);
void NpcOnRespawn(int npc_id);
void PlayerOnDeath(std::shared_ptr<Player> session);
void PlayerOnHpRegen(int client_id);
void BroadcastHpToParty(const std::shared_ptr<Player>& session);
void PlayerOnMpRegen(int client_id);
void OnPlayerSpawn(int client_id, const PlayerSnapshot& snap, bool exists);
void OnDbResponse(DbResponse& resp);
void PlayerOnAutoSave();
PlayerSnapshot SnapshotPlayer(const std::shared_ptr<Player>& session);
void LevelUpPlayer(std::shared_ptr<Player> session, unsigned long long exp_gain);
void GiveExpToKillerAndParty(std::shared_ptr<Player> killer, unsigned long long exp_gain);
void PlayerLeaveParty(std::shared_ptr<Player> session);
void SendSystemMessage(std::shared_ptr<Player> session, const std::string& msg);
void SpawnNpcLoot(NpcType type, NpcMoveMode mode, int level, short x, short y);
void OnGroundItemExpire(int drop_id);
void OnPartyInviteExpire(int invitee_id);
void SendInventory(const std::shared_ptr<Player>& session);
void SendQuestUpdate(const std::shared_ptr<Player>& session, int quest_id, int kill_count, int target_count, unsigned char state);
void SendAllQuestUpdates(const std::shared_ptr<Player>& session);
void OnNpcKilledForQuest(const std::shared_ptr<Player>& killer, const NPC& n);

// 플레이어 시점에서 자기 시야 안의 NPC와 view_list를 동기화.
// 새로 시야에 들어온 NPC: Add 전송 + 양방향 view 등록 + Lazy AI 활성
// 시야에서 벗어난 NPC: Remove 전송 + 양방향 view 해제
// 유지된 NPC: 아무 것도 하지 않음 (NPC가 직접 Move 패킷을 보냄)
void SyncPlayerNpcView(shared_ptr<Player> player) {
    int sx = player->x / SECTOR_SIZE;
    int sy = player->y / SECTOR_SIZE;

    vector<int> candidate_nids;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = sx + dx, ny = sy + dy;
            if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
            lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
            for (int nid : g_sectors[ny][nx].npcs) candidate_nids.push_back(nid);
        }
    }

    unordered_set<int> new_view;
    for (int nid : candidate_nids) {
        NPC& n = GetNpc(nid);
        if (IsInView(player->x, player->y, n.x, n.y)) {
            new_view.insert(nid);
        }
    }

    vector<int> entered, left;
    {
        lock_guard<mutex> lock(player->view_lock);
        for (int nid : new_view) {
            if (player->view_list.count(nid) == 0) entered.push_back(nid);
        }
        for (int id : player->view_list) {
            if (!IsNpcId(id)) continue;          // player diff는 별도 경로에서 처리
            if (new_view.count(id) == 0) left.push_back(id);
        }
        for (int nid : entered) player->view_list.insert(nid);
        for (int nid : left)    player->view_list.erase(nid);
    }

    for (int nid : entered) {
        NPC& n = GetNpc(nid);
        {
            lock_guard<mutex> lk(n.view_lock);
            n.view_list.insert(player->id);
        }
        SendAddNpc(player, n);

        // Lazy AI: 비활성 상태였다면 활성화 + 첫 타이머 등록 (CAS로 중복 등록 방지)
        bool expected = false;
        if (n.active.compare_exchange_strong(expected, true)) {
            g_timer_manager.Schedule(nid, TimerEventKind::NpcMove, NPC_TICK_INTERVAL_MS);
        }
    }
    for (int nid : left) {
        NPC& n = GetNpc(nid);
        {
            lock_guard<mutex> lk(n.view_lock);
            n.view_list.erase(player->id);
        }
        SendRemoveObject(player, nid);
        // 비활성화는 NPC가 다음 tick에서 view_list 비었을 때 스스로 수행 (race-free)
    }
}

// NPC AI tick. TimerEventKind::NpcMove 완료 시 worker_thread에서 호출.
// AI 결정(이동 방향 / target 갱신)은 Lua의 OnTick에 위임. C++는 결과를 받아
// 월드 상태(sector, view_list, 패킷 송신)만 갱신.
void NpcOnMove(int npc_id) {
    NPC& npc = GetNpc(npc_id);
    if (npc.state == NpcFsmState::Dead) {
        npc.active.store(false);
        return;
    }

    short old_x = npc.x;
    short old_y = npc.y;

    // 1) view_list snapshot + 가장 가까운 player(chebyshev) 계산
    vector<int> viewer_snapshot;
    {
        lock_guard<mutex> lk(npc.view_lock);
        viewer_snapshot.assign(npc.view_list.begin(), npc.view_list.end());
    }
    int nearest_id = -1;
    int nearest_dist = -1;
    short nearest_x = -1, nearest_y = -1;
    short target_x = -1, target_y = -1;  // 현재 추적 대상의 위치
    int cur_target_id = npc.target_id;
    for (int pid : viewer_snapshot) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, pid)) continue;
        short px = a->second->x;
        short py = a->second->y;
        // Chebyshev distance (PDF의 11x11/15x15는 정사각형 영역)
        int d = max(abs(px - old_x), abs(py - old_y));
        if (nearest_id == -1 || d < nearest_dist) {
            nearest_id = pid;
            nearest_dist = d;
            nearest_x = px;
            nearest_y = py;
        }
        if (pid == cur_target_id) {
            target_x = px;
            target_y = py;
        }
    }
    // target이 시야에 없으면 target_x/y = -1 그대로 → Lua가 추적 해제 결정

    // 2) Lua AI tick 호출
    NpcTickContext ctx;
    ctx.id = npc_id;
    ctx.npc_type = static_cast<int>(npc.type);
    ctx.move_mode = static_cast<int>(npc.move_mode);
    ctx.x = old_x;
    ctx.y = old_y;
    ctx.spawn_x = npc.spawn_x;
    ctx.spawn_y = npc.spawn_y;
    ctx.target_id = cur_target_id;
    ctx.target_x = target_x;
    ctx.target_y = target_y;
    ctx.nearest_id = nearest_id;
    ctx.nearest_x = nearest_x;
    ctx.nearest_y = nearest_y;
    ctx.nearest_dist = nearest_dist;
    ctx.hp = npc.hp;
    ctx.max_hp = npc.max_hp;
    ctx.boss_tick_count = npc.boss_tick_count.load();

    NpcTickResult result;
    LuaVM& lua = GetWorkerLua();  // [perf] 워커 로컬 VM — 전역 Lua 락 직렬화 제거
    if (npc.type == NpcType::Boss) {
        lua.BossTick(ctx, result);
        npc.boss_tick_count.fetch_add(1);
    }
    else {
        lua.NpcTick(ctx, result);  // 실패해도 result는 안전 디폴트(정지 + target 유지)
    }

    // 3) 결과 적용: target_id 갱신 + state 전환
    int new_target = result.target_id;
    if (new_target != cur_target_id) {
        npc.target_id = new_target;
    }
    if (new_target != -1) {
        npc.state = NpcFsmState::Chasing;
    }
    else {
        npc.state = (npc.move_mode == NpcMoveMode::Roaming)
            ? NpcFsmState::Roaming : NpcFsmState::Idle;
    }

    // 3.5) Agro 타겟이 카디널 인접(manhattan==1)에 있으면 이동 대신 공격
    // Lua에서 would_step_onto 가드로 dx=dy=0 반환되었으므로 NPC는 이미 정지 상태.
    if (new_target != -1) {
        // 타겟 좌표 결정: cur_target_id 유지 → target_x/y, Agro 트리거 → nearest_x/y
        short tgt_x = -1, tgt_y = -1;
        if (new_target == cur_target_id && target_x != -1) {
            tgt_x = target_x; tgt_y = target_y;
        }
        else if (new_target == nearest_id) {
            tgt_x = nearest_x; tgt_y = nearest_y;
        }

        if (tgt_x != -1) {
            int dxa = std::abs(static_cast<int>(tgt_x) - static_cast<int>(old_x));
            int dya = std::abs(static_cast<int>(tgt_y) - static_cast<int>(old_y));
            if (dxa + dya == 1) {
                // 카디널 인접 — NPC 공격 쿨타임 검사
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                long long last_atk = npc.last_attack_ms.load();
                if (last_atk == 0 || now_ms - last_atk >= NPC_ATTACK_INTERVAL_MS) {
                    npc.last_attack_ms.store(now_ms);

                    // 공격 방향 (타겟 쪽)
                    unsigned char atk_dir = 0;
                    if (tgt_x > old_x)      atk_dir = 2; // Right
                    else if (tgt_x < old_x) atk_dir = 1; // Left
                    else if (tgt_y > old_y) atk_dir = 0; // Down
                    else                    atk_dir = 3; // Up

                    // 타겟 플레이어에게 데미지 적용 (compare_exchange로 동시 공격 보호)
                    shared_ptr<Player> target_player;
                    {
                        ClientMap::const_accessor a;
                        if (g_clients.find(a, new_target)) target_player = a->second;
                    }
                    if (target_player) {
                        int damage = static_cast<int>(npc.level) * NPC_BASE_DAMAGE;
                        if (damage < 1) damage = 1;
                        int old_hp = target_player->hp.load();
                        int new_hp_val = 0;
                        bool dealt = false;
                        while (old_hp > 0) {
                            int next = (old_hp > damage) ? (old_hp - damage) : 0;
                            if (target_player->hp.compare_exchange_weak(old_hp, next)) {
                                new_hp_val = next;
                                dealt = true;
                                break;
                            }
                        }

                        if (dealt) {
                            // S2C_ATTACK_ANIM 브로드캐스트 (NPC view_list 안 플레이어 + 타겟)
                            S2C_AttackAnim anim;
                            anim.size = sizeof(anim);
                            anim.type = S2C_ATTACK_ANIM;
                            anim.object_id = npc_id;
                            anim.direction = atk_dir;

                            unordered_set<int> atk_viewers;
                            {
                                lock_guard<mutex> lock(npc.view_lock);
                                for (int vid : npc.view_list) atk_viewers.insert(vid);
                            }
                            atk_viewers.insert(new_target);
                            for (int vid : atk_viewers) {
                                ClientMap::const_accessor a;
                                if (g_clients.find(a, vid)) a->second->do_send(anim.size, &anim);
                            }

                            // S2C_DAMAGE (타겟 + 시야 내 다른 플레이어)
                            S2C_Damage dmg;
                            dmg.size = sizeof(dmg);
                            dmg.type = S2C_DAMAGE;
                            dmg.attacker_id = npc_id;
                            dmg.target_id = new_target;
                            dmg.damage = damage;
                            dmg.new_hp = new_hp_val;
                            dmg.target_x = target_player->x;
                            dmg.target_y = target_player->y;
                            for (int vid : atk_viewers) {
                                ClientMap::const_accessor a;
                                if (g_clients.find(a, vid)) a->second->do_send(dmg.size, &dmg);
                            }

                            // 사망 체크 — Lazy AI 안 비활성화 사이클을 깨지 않도록 타이머 재스케줄 후 호출
                            if (new_hp_val == 0) {
                                PlayerOnDeath(target_player);  // 부활 후 풀HP를 파티원에 브로드캐스트
                            } else {
                                // S2C_DAMAGE는 시야 기반이라 시야 밖 파티원 HP바가 안 줄어든다 → 직접 동기화
                                BroadcastHpToParty(target_player);
                            }
                        }
                    }
                }
            }
        }
    }

    // 4) 이동 적용 + 월드 경계 클램프 + 장애물 회피
    short new_x = static_cast<short>(old_x + result.dx);
    short new_y = static_cast<short>(old_y + result.dy);
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x >= WORLD_WIDTH)  new_x = WORLD_WIDTH - 1;
    if (new_y >= WORLD_HEIGHT) new_y = WORLD_HEIGHT - 1;

    // 막힌 타일로 향하면 A*로 우회. 추격 중: 타겟 방향. 로밍 중: 스폰 방향.
    if (Map::IsBlocked(new_x, new_y)) {
        bool detoured = false;
        if (new_target != -1) {
            short tgt_x_path = -1, tgt_y_path = -1;
            if (new_target == cur_target_id && target_x != -1) {
                tgt_x_path = target_x;
                tgt_y_path = target_y;
            }
            else if (new_target == nearest_id) {
                tgt_x_path = nearest_x;
                tgt_y_path = nearest_y;
            }
            if (tgt_x_path != -1) {
                int dx_alt = 0, dy_alt = 0;
                if (AStar::AStarStep(old_x, old_y, tgt_x_path, tgt_y_path, dx_alt, dy_alt)
                    && (dx_alt != 0 || dy_alt != 0)) {
                    int alt_x = static_cast<int>(old_x) + dx_alt;
                    int alt_y = static_cast<int>(old_y) + dy_alt;
                    if (std::abs(alt_x - static_cast<int>(npc.spawn_x)) <= ROAM_AREA_RANGE
                        && std::abs(alt_y - static_cast<int>(npc.spawn_y)) <= ROAM_AREA_RANGE
                        && !Map::IsBlocked(alt_x, alt_y)) {
                        new_x = static_cast<short>(alt_x);
                        new_y = static_cast<short>(alt_y);
                        detoured = true;
                    }
                }
            }
        }
        // 로밍 NPC가 장애물에 막혔을 때 A*로 스폰 방향 우회
        if (!detoured && npc.move_mode == NpcMoveMode::Roaming) {
            int dx_alt = 0, dy_alt = 0;
            if (AStar::AStarStep(old_x, old_y, npc.spawn_x, npc.spawn_y, dx_alt, dy_alt)
                && (dx_alt != 0 || dy_alt != 0)) {
                int alt_x = static_cast<int>(old_x) + dx_alt;
                int alt_y = static_cast<int>(old_y) + dy_alt;
                if (std::abs(alt_x - static_cast<int>(npc.spawn_x)) <= ROAM_AREA_RANGE
                    && std::abs(alt_y - static_cast<int>(npc.spawn_y)) <= ROAM_AREA_RANGE
                    && !Map::IsBlocked(alt_x, alt_y)) {
                    new_x = static_cast<short>(alt_x);
                    new_y = static_cast<short>(alt_y);
                    detoured = true;
                }
            }
        }
        if (!detoured) {
            new_x = old_x;
            new_y = old_y;
        }
    }

    bool moved = (new_x != old_x || new_y != old_y);
    if (moved) {
        UpdateObjectSector(npc_id, old_x, old_y, new_x, new_y, false);
        npc.x = new_x;
        npc.y = new_y;
    }

    // 새 위치 주변 9섹터의 플레이어 후보 수집
    int sx = new_x / SECTOR_SIZE;
    int sy = new_y / SECTOR_SIZE;
    vector<int> candidate_pids;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = sx + dx, ny = sy + dy;
            if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
            lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
            for (int pid : g_sectors[ny][nx].players) candidate_pids.push_back(pid);
        }
    }

    unordered_map<int, shared_ptr<Player>> id_to_session;
    unordered_set<int> new_view;
    for (int pid : candidate_pids) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, pid)) continue;
        auto& p = a->second;
        if (IsInView(new_x, new_y, p->x, p->y)) {
            new_view.insert(pid);
            id_to_session[pid] = p;
        }
    }

    vector<int> entered, left, stayed;
    {
        lock_guard<mutex> lock(npc.view_lock);
        for (int pid : new_view) {
            if (npc.view_list.count(pid)) stayed.push_back(pid);
            else entered.push_back(pid);
        }
        for (int pid : npc.view_list) {
            if (new_view.count(pid) == 0) left.push_back(pid);
        }
        // 덮어쓰기 대신 증분 적용: SyncPlayerNpcView가 락 밖에서 삽입한 항목을 보존
        for (int pid : left)    npc.view_list.erase(pid);
        for (int pid : entered) npc.view_list.insert(pid);
    }

    // entered: player view에 NPC 추가 + Add 전송
    for (int pid : entered) {
        auto it = id_to_session.find(pid);
        if (it == id_to_session.end()) continue;
        auto& p = it->second;
        {
            lock_guard<mutex> lk(p->view_lock);
            p->view_list.insert(npc_id);
        }
        SendAddNpc(p, npc);
    }
    // left: player view에서 NPC 제거 + Remove 전송
    for (int pid : left) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, pid)) continue;
        auto& p = a->second;
        {
            lock_guard<mutex> lk(p->view_lock);
            p->view_list.erase(npc_id);
        }
        SendRemoveObject(p, npc_id);
    }
    // stayed: 이동했으면 Move 전송
    if (moved) {
        S2C_MoveObject pkt;
        pkt.size = sizeof(pkt);
        pkt.type = S2C_MOVE_OBJECT;
        pkt.object_id = npc_id;
        pkt.x = new_x;
        pkt.y = new_y;
        pkt.move_time = NPC_TICK_INTERVAL_MS;
        for (int pid : stayed) {
            auto it = id_to_session.find(pid);
            if (it == id_to_session.end()) continue;
            it->second->do_send(pkt.size, &pkt);
        }
    }

    // ==== 보스 전용 처리: 채팅 + AoE ====
    if (npc.type == NpcType::Boss) {
        // 보스 채팅: 시야 내 플레이어뿐 아니라 주변 3섹터 범위(약 60타일)까지 브로드캐스트
        if (result.chat_id > 0) {
            const char* boss_msgs[] = {
                "You dare enter my domain?!",
                "I will crush you!",
                "None shall pass!",
                "ROARRR!!"
            };
            int mid = result.chat_id - 1;  // Lua는 1~4 반환, 배열은 0~3
            if (mid >= 0 && mid <= 3) {
                S2C_ChatMessage cpkt;
                cpkt.size = static_cast<unsigned char>(sizeof(cpkt));
                cpkt.type = S2C_CHAT_MESSAGE;
                cpkt.object_id = npc_id;
                strncpy_s(cpkt.message, boss_msgs[mid], _TRUNCATE);
                // 3섹터 반경(60타일) 내 플레이어에게 전송 — 보스 위협 연출
                int bsx = new_x / SECTOR_SIZE, bsy = new_y / SECTOR_SIZE;
                for (int ddy = -3; ddy <= 3; ++ddy) {
                    for (int ddx = -3; ddx <= 3; ++ddx) {
                        int nx = bsx + ddx, ny = bsy + ddy;
                        if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
                        vector<int> pids_in_sector;
                        {
                            lock_guard<mutex> lk(g_sectors[ny][nx].m_lock);
                            pids_in_sector.assign(g_sectors[ny][nx].players.begin(),
                                                  g_sectors[ny][nx].players.end());
                        }
                        for (int pid : pids_in_sector) {
                            ClientMap::const_accessor a;
                            if (g_clients.find(a, pid)) a->second->do_send(cpkt.size, &cpkt);
                        }
                    }
                }
            }
        }

        // 보스 AoE (do_boss_aoe == 1이면 BOSS_AOE_RANGE 반경 내 모든 플레이어에 데미지)
        if (result.do_boss_aoe != 0) {
            for (auto& [pid, psess] : id_to_session) {
                int ddx = std::abs(static_cast<int>(psess->x) - static_cast<int>(new_x));
                int ddy = std::abs(static_cast<int>(psess->y) - static_cast<int>(new_y));
                if (std::max(ddx, ddy) > BOSS_AOE_RANGE) continue;

                int damage = BOSS_BASE_DAMAGE;
                int old_hp = psess->hp.load();
                int new_hp_v = 0;
                bool dealt = false;
                while (old_hp > 0) {
                    int next = (old_hp > damage) ? (old_hp - damage) : 0;
                    if (psess->hp.compare_exchange_weak(old_hp, next)) {
                        new_hp_v = next;
                        dealt = true;
                        break;
                    }
                }
                if (dealt) {
                    // 피격 패킷 (시야 내 전원 + 피격자)
                    S2C_Damage dpkt;
                    dpkt.size = sizeof(dpkt);
                    dpkt.type = S2C_DAMAGE;
                    dpkt.attacker_id = npc_id;
                    dpkt.target_id = pid;
                    dpkt.damage = damage;
                    dpkt.new_hp = new_hp_v;
                    dpkt.target_x = psess->x;
                    dpkt.target_y = psess->y;
                    for (auto& [vid, vsess] : id_to_session) {
                        vsess->do_send(dpkt.size, &dpkt);
                    }
                    psess->do_send(dpkt.size, &dpkt);

                    // 플레이어 사망 처리
                    if (new_hp_v == 0) {
                        PlayerOnDeath(psess);  // 부활 후 풀HP를 파티원에 브로드캐스트
                    } else {
                        // S2C_DAMAGE는 시야 기반이라 시야 밖 파티원 HP바가 안 줄어든다 → 직접 동기화
                        BroadcastHpToParty(psess);
                    }
                }
            }
        }
    }

    // 시야가 비었으면 비활성화, 아니면 재스케줄
    bool has_viewer;
    {
        lock_guard<mutex> lock(npc.view_lock);
        has_viewer = !npc.view_list.empty();
        if (!has_viewer) npc.active.store(false);
    }
    if (has_viewer) {
        g_timer_manager.Schedule(npc_id, TimerEventKind::NpcMove, NPC_TICK_INTERVAL_MS);
    }
}

// NPC 리스폰. 30초 사망 타이머 만료 시 worker_thread에서 호출.
// spawn_x/spawn_y로 위치 초기화, HP 풀회복, 섹터 재등록, 시야 내 플레이어에게 Add+Respawn 송신.
void NpcOnRespawn(int npc_id) {
    NPC& npc = GetNpc(npc_id);

    // 1) 상태 리셋 (락 안에서 일관성 보장)
    short rx, ry;
    {
        lock_guard<mutex> lk(npc.view_lock);
        npc.hp = npc.max_hp;
        npc.x = npc.spawn_x;
        npc.y = npc.spawn_y;
        npc.target_id = -1;
        npc.state = (npc.move_mode == NpcMoveMode::Roaming) ? NpcFsmState::Roaming : NpcFsmState::Idle;
        rx = npc.x;
        ry = npc.y;
    }
    npc.boss_tick_count.store(0);  // 보스 리스폰 시 틱/페이즈 초기화
    npc.active.store(false);

    // 2) 섹터 재등록 (이전엔 RemoveObjectFromSector로 빠져있음)
    UpdateObjectSector(npc_id, -1, -1, rx, ry, false);

    // 3) 리스폰 위치의 시야(9섹터) 내 플레이어 찾아서 Add+Respawn 송신 + 상호 view 등록
    int sx = rx / SECTOR_SIZE;
    int sy = ry / SECTOR_SIZE;
    vector<int> candidate_pids;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = sx + dx, ny = sy + dy;
            if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
            lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
            for (int pid : g_sectors[ny][nx].players) candidate_pids.push_back(pid);
        }
    }

    bool any_viewer = false;
    for (int pid : candidate_pids) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, pid)) continue;
        auto& player = a->second;
        if (!IsInView(rx, ry, player->x, player->y)) continue;

        // 양방향 view_list 등록
        {
            lock_guard<mutex> lk(player->view_lock);
            player->view_list.insert(npc_id);
        }
        {
            lock_guard<mutex> lk(npc.view_lock);
            npc.view_list.insert(pid);
        }

        // S2C_ADD_OBJECT (NPC를 화면에 다시 추가) + S2C_RESPAWN (pillar 이펙트)
        SendAddNpc(player, npc);

        S2C_Respawn rpkt;
        rpkt.size = sizeof(rpkt);
        rpkt.type = S2C_RESPAWN;
        rpkt.object_id = npc_id;
        rpkt.respawn_x = rx;
        rpkt.respawn_y = ry;
        rpkt.hp = npc.max_hp;
        rpkt.max_hp = npc.max_hp;
        player->do_send(rpkt.size, &rpkt);

        any_viewer = true;
    }

    // 4) 시야에 플레이어가 있으면 AI 재가동
    if (any_viewer) {
        npc.active.store(true);
        g_timer_manager.Schedule(npc_id, TimerEventKind::NpcMove, NPC_TICK_INTERVAL_MS);
    }
    // else: Lazy AI — 플레이어가 가까이 오면 SyncPlayerNpcView가 active=true로 만들고 첫 NpcMove 등록
}

// 플레이어 사망 처리: EXP 50% 손실 + spawn 위치 텔레포트 + HP 회복 + 시야 재구성.
// 호출 시점: NPC 공격으로 HP가 0이 된 직후 (NpcOnMove 안에서)
void PlayerOnDeath(std::shared_ptr<Player> session) {
    int client_id = session->id;
    short death_x = session->x;
    short death_y = session->y;

    // 1) 현재 시야의 다른 플레이어 스냅샷 (NPC 제외)
    vector<int> old_viewers;
    {
        lock_guard<mutex> lk(session->view_lock);
        for (int id : session->view_list) {
            if (!IsNpcId(id)) old_viewers.push_back(id);
        }
    }

    // 2) S2C_DEATH 송신 (death_x/y에서 soul 이펙트) — 자기 자신 + 시야 내 다른 플레이어
    S2C_Death dpkt;
    dpkt.size = sizeof(dpkt);
    dpkt.type = S2C_DEATH;
    dpkt.object_id = client_id;
    dpkt.death_x = death_x;
    dpkt.death_y = death_y;
    session->do_send(dpkt.size, &dpkt);
    for (int pid : old_viewers) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, pid)) a->second->do_send(dpkt.size, &dpkt);
    }

    // 3) Old viewers 쪽에서 본 플레이어 제거 (REMOVE_OBJECT) + 자기 view_list 정리
    for (int pid : old_viewers) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, pid)) continue;
        {
            lock_guard<mutex> lk(a->second->view_lock);
            a->second->view_list.erase(client_id);
        }
        SendRemoveObject(a->second, client_id);
    }
    // 자기 view_list: NPC들의 view_list에서 먼저 제거한 뒤 전체 비움.
    // 제거하지 않으면 NPC가 리스폰 위치의 플레이어에게 계속 Move/Attack을 전송해
    // 클라이언트 화면에 보이지 않는 NPC의 패킷이 수신된다(유령 NPC).
    {
        vector<int> npc_ids;
        {
            lock_guard<mutex> lk(session->view_lock);
            for (int id : session->view_list)
                if (IsNpcId(id)) npc_ids.push_back(id);
            session->view_list.clear();
        }
        for (int nid : npc_ids) {
            int idx = nid - NPC_ID_START;
            if (idx < 0 || idx >= g_npc_count) continue;
            lock_guard<mutex> lk(g_npcs[idx].view_lock);
            g_npcs[idx].view_list.erase(client_id);
        }
    }

    // 4) 사망 페널티: EXP 50% 손실, HP 풀회복
    unsigned long long lost_exp = session->exp.load() / 2;
    session->exp.fetch_sub(lost_exp);
    session->hp.store(session->max_hp.load());

    // 5) 시작 위치로 텔레포트 + 섹터 갱신
    // move_lock으로 HandleMove와 경쟁 차단: 현재 좌표를 락 안에서 재독해 섹터 이동의 출발점으로
    // 삼아야, 사망 직전 이동으로 death_x/y가 stale해도 올바른 섹터에서 빠져나간다.
    short respawn_x = PLAYER_SPAWN_X;
    short respawn_y = PLAYER_SPAWN_Y;
    {
        lock_guard<mutex> mlk(session->move_lock);
        short cur_x = session->x, cur_y = session->y;
        UpdateObjectSector(client_id, cur_x, cur_y, respawn_x, respawn_y, true);
        session->x = respawn_x;
        session->y = respawn_y;
    }

    // 6) 새 위치 9섹터에서 신규 viewer 후보 수집 → IsInView 필터 → 양방향 등록 + Add 패킷
    int sx = respawn_x / SECTOR_SIZE;
    int sy = respawn_y / SECTOR_SIZE;
    vector<int> new_candidates;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = sx + dx, ny = sy + dy;
            if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
            lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
            for (int pid : g_sectors[ny][nx].players) {
                if (pid != client_id) new_candidates.push_back(pid);
            }
        }
    }
    vector<shared_ptr<Player>> new_viewer_sessions;
    for (int pid : new_candidates) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, pid)) continue;
        auto& other = a->second;
        if (!IsInView(respawn_x, respawn_y, other->x, other->y)) continue;

        {
            lock_guard<mutex> lk(session->view_lock);
            session->view_list.insert(pid);
        }
        {
            lock_guard<mutex> lk(other->view_lock);
            other->view_list.insert(client_id);
        }
        SendAddObject(other, session);   // 다른 플레이어 화면에 부활자 추가
        SendAddObject(session, other);   // 부활자 화면에 다른 플레이어 추가
        new_viewer_sessions.push_back(other);
    }

    // 7) S2C_RESPAWN 송신 (pillar 이펙트) — 자기 자신 + 신규 viewer
    S2C_Respawn rpkt;
    rpkt.size = sizeof(rpkt);
    rpkt.type = S2C_RESPAWN;
    rpkt.object_id = client_id;
    rpkt.respawn_x = respawn_x;
    rpkt.respawn_y = respawn_y;
    rpkt.hp = session->hp.load();
    rpkt.max_hp = session->max_hp.load();
    session->do_send(rpkt.size, &rpkt);
    for (auto& other : new_viewer_sessions) {
        other->do_send(rpkt.size, &rpkt);
    }

    // 8) 자기 자신 + 파티원에게 스탯 변경 통보 (본인 HUD / 파티원 HP바 갱신)
    //    S2C_DEATH/RESPAWN은 둘 다 시야 기반이라 시야 밖 파티원은 사망·부활을 못 받는다.
    //    부활로 풀회복된 HP를 시야 밖 파티원 HP바에도 반영하려면 S2C_StatusChange를
    //    파티 전체에 보내야 한다(SendStatusChange와 동일 패턴). 누락 시 HP바가 사망 직전 값에 멈춤.
    S2C_StatusChange sc;
    sc.size = sizeof(sc);
    sc.type = S2C_STATUS_CHANGE;
    sc.object_id = client_id;
    sc.hp = session->hp.load();
    sc.max_hp = session->max_hp.load();
    sc.exp = session->exp.load();
    sc.level = session->level.load();
    session->do_send(sc.size, &sc);
    BroadcastHpToParty(session);  // 시야 밖 파티원 HP바를 부활 후 풀HP로 갱신

    // 9) NPC 시야 동기화 (새 위치 주변)
    SyncPlayerNpcView(session);
}

// 5초마다 호출되는 HP 회복 틱. max_hp의 HP_REGEN_PERCENT(10%)만큼 회복하고 재스케줄.
// 플레이어가 disconnect 되었으면 g_clients에서 못 찾아 체인이 끊김 (자연 정리).
void PlayerOnHpRegen(int client_id) {
    std::shared_ptr<Player> session;
    {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, client_id)) return;  // 접속 종료된 세션 — 재스케줄 안 함
        session = a->second;
    }

    int max_hp = session->max_hp.load();
    int cur_hp = session->hp.load();

    // 사망 직후가 아니고 HP가 최대치 미만일 때만 회복
    if (cur_hp > 0 && cur_hp < max_hp) {
        int heal = max_hp * HP_REGEN_PERCENT / 100;
        if (heal < 1) heal = 1;

        int observed = cur_hp;
        while (true) {
            int next = observed + heal;
            if (next > max_hp) next = max_hp;
            if (session->hp.compare_exchange_weak(observed, next)) {
                cur_hp = next;
                break;
            }
            if (observed <= 0 || observed >= max_hp) break;  // 그 사이 변동되면 중단
        }

        // 자기 자신에게 스탯 변경 통보 (HUD 갱신)
        S2C_StatusChange sc;
        sc.size = sizeof(sc);
        sc.type = S2C_STATUS_CHANGE;
        sc.object_id = client_id;
        sc.hp = cur_hp;
        sc.max_hp = max_hp;
        sc.exp = session->exp.load();
        sc.level = session->level.load();
        session->do_send(sc.size, &sc);

        // 시야 밖 파티원의 HP바도 회복분만큼 따라오도록 동기화
        BroadcastHpToParty(session);
    }

    // 다음 회복 tick 재스케줄
    g_timer_manager.Schedule(client_id, TimerEventKind::HpRegen, HP_REGEN_INTERVAL_MS);
}

// MP 고속 재생 틱 (MP_REGEN_INTERVAL_MS마다). 변경이 있을 때만 본인에게 S2C_MpChange 전송.
void PlayerOnMpRegen(int client_id) {
    std::shared_ptr<Player> session;
    {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, client_id)) return;  // 접속 종료 — 체인 종료
        session = a->second;
    }

    int max_mp = session->max_mp.load();
    int cur_mp = session->mp.load();
    if (cur_mp < max_mp) {
        int next = cur_mp + MP_REGEN_AMOUNT;
        if (next > max_mp) next = max_mp;
        session->mp.store(next);

        S2C_MpChange mc;
        mc.size = sizeof(mc);
        mc.type = S2C_MP_CHANGE;
        mc.object_id = client_id;
        mc.mp = next;
        mc.max_mp = max_mp;
        session->do_send(mc.size, &mc);
    }

    g_timer_manager.Schedule(client_id, TimerEventKind::MpRegen, MP_REGEN_INTERVAL_MS);
}

// Stage 6.3: 현재 플레이어 상태를 PlayerSnapshot으로 캡쳐 (Save용)
PlayerSnapshot SnapshotPlayer(const std::shared_ptr<Player>& session) {
    PlayerSnapshot snap;
    auto name_ptr = atomic_load(&session->name);
    snap.username = name_ptr ? *name_ptr : std::string{};
    snap.hp        = session->hp.load();
    snap.exp       = session->exp.load();
    snap.level     = session->level.load();
    // 좌표는 move_lock 하에서 읽어 HandleMove/PlayerOnDeath의 락 구간 좌표 쓰기와의
    // 데이터 레이스(new-x + old-y로 찢어진 좌표 저장)를 막는다. move_lock은 최외곽 락이므로
    // inv_lock/quest_lock보다 먼저, 좁은 스코프에서 잡았다 푼다(락 순서 보존).
    {
        lock_guard<mutex> mlk(session->move_lock);
        snap.x = session->x;
        snap.y = session->y;
    }
    snap.direction = session->direction.load();
    // Stage 8: inv_lock 보유 중에 장비+max_hp 일괄 읽기 — 착탈 race 방지
    // EquipItem/UnequipItem 모두 inv_lock 하에서 equipped_*와 max_hp를 갱신하므로
    // 동일 락으로 읽으면 armor_bonus와 max_hp가 항상 일관된 상태를 반영한다.
    {
        lock_guard<mutex> lk(session->inv_lock);
        int armor_id = session->equipped_armor_id.load();
        const ItemDef* adef = (armor_id >= 0) ? GetItemDef(armor_id) : nullptr;
        int armor_bonus = adef ? adef->value : 0;
        snap.max_hp = session->max_hp.load() - armor_bonus;
        if (snap.max_hp < 1) snap.max_hp = 1;
        snap.equipped_weapon_id = session->equipped_weapon_id.load();
        snap.equipped_armor_id  = armor_id;
        snap.inventory = session->inventory;
    }
    // Stage 9: 퀘스트 진행/완료 상태 저장
    {
        lock_guard<mutex> lk(session->quest_lock);
        snap.quests.reserve(session->quests.size());
        for (const auto& q : session->quests)
            snap.quests.push_back({ q.quest_id, q.kill_count, static_cast<int>(q.state) });
    }
    return snap;
}

// LOGIN 흐름의 후반부 (DB Load 응답 도착 후): spawn 위치 결정 + S2C_AvatarInfo + view 구축 + HpRegen 시작
void OnPlayerSpawn(int client_id, const PlayerSnapshot& snap, bool exists) {
    shared_ptr<Player> session;
    {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, client_id)) return;  // 응답 도착 전 클라가 끊김
        session = a->second;
    }

    // 스폰 위치 결정:
    //  - 기존 캐릭: 마지막 로그아웃 위치(snap.x/y)를 복원 — DB에 저장된 좌표 사용.
    //    범위 밖 / blocked / 미저장(0,0) 이면 마을로 폴백.
    //  - 신규 캐릭(또는 폴백): Aetheria Village 영역 안 walkable 무작위 좌표.
    short sx_pos = PLAYER_SPAWN_X;
    short sy_pos = PLAYER_SPAWN_Y;
    bool restored_pos = false;
    if (exists &&
        !(snap.x == 0 && snap.y == 0) &&
        snap.x >= 0 && snap.x < WORLD_WIDTH &&
        snap.y >= 0 && snap.y < WORLD_HEIGHT &&
        !Map::IsBlocked(snap.x, snap.y)) {
        sx_pos = snap.x;
        sy_pos = snap.y;
        restored_pos = true;
    }
    if (!restored_pos) {
        int range_x = VILLAGE_X2 - VILLAGE_X1;
        int range_y = VILLAGE_Y2 - VILLAGE_Y1;
        for (int attempt = 0; attempt < 32; ++attempt) {
            short tx = static_cast<short>(VILLAGE_X1 + t_npc_rng() % range_x);
            short ty = static_cast<short>(VILLAGE_Y1 + t_npc_rng() % range_y);
            if (!Map::IsBlocked(tx, ty)) { sx_pos = tx; sy_pos = ty; break; }
        }
    }
    if (exists) {
        int base_max = snap.max_hp > 0 ? snap.max_hp : 100;

        // Stage 8: 장착 복원 (카탈로그에 없는 아이템이면 해제 처리)
        int weapon = snap.equipped_weapon_id;
        int armor  = snap.equipped_armor_id;
        const ItemDef* wdef = (weapon >= 0) ? GetItemDef(weapon) : nullptr;
        const ItemDef* adef = (armor  >= 0) ? GetItemDef(armor)  : nullptr;
        if (!wdef && weapon >= 0) {
            cout << "[Warn] " << snap.username << " had weapon id=" << weapon
                 << " which is no longer in catalog — unequipped." << endl;
            weapon = -1;
        }
        if (!adef && armor >= 0) {
            cout << "[Warn] " << snap.username << " had armor id=" << armor
                 << " which is no longer in catalog — unequipped." << endl;
            armor = -1;
        }
        int armor_bonus = adef ? adef->value : 0;
        int eff_max = base_max + armor_bonus;

        session->max_hp.store(eff_max);
        int loaded_hp = snap.hp > 0 ? snap.hp : eff_max;
        if (loaded_hp > eff_max) loaded_hp = eff_max;
        session->hp.store(loaded_hp);
        session->exp.store(snap.exp);
        session->level.store(snap.level > 0 ? snap.level : 1);
        session->equipped_weapon_id.store(weapon);
        session->equipped_armor_id.store(armor);
        session->atk_bonus.store(wdef ? wdef->value : 0);
        session->direction.store(snap.direction);
        { lock_guard<mutex> lk(session->inv_lock); session->inventory = snap.inventory; }
        // Stage 9: 퀘스트 진행/완료 상태 복원 (def가 없는 퀘스트는 스킵)
        {
            lock_guard<mutex> lk(session->quest_lock);
            session->quests.clear();
            for (const auto& q : snap.quests) {
                if (!GetQuestDef(q[0])) continue;
                unsigned char st = (q[2] != 0) ? 1 : 0;
                session->quests.push_back({ q[0], q[1], st });
            }
        }
    }
    // 신규 캐릭은 atomic 기본값 (Player ctor: hp=100, max_hp=100, exp=0, level=1, 빈 인벤) 그대로 사용

    session->x = sx_pos;
    session->y = sy_pos;
    UpdateObjectSector(client_id, -1, -1, session->x, session->y, true);

    // MP는 영속하지 않으므로 로그인 시 가득 채워 시작
    session->max_mp.store(MP_MAX);
    session->mp.store(MP_MAX);

    // 1. 본인에게 아바타 정보 전송 (DB에서 복원된 hp/exp/level 반영)
    S2C_AvatarInfo info;
    info.size = sizeof(info);
    info.type = S2C_AVATAR_INFO;
    info.playerId = client_id;
    info.x = session->x;
    info.y = session->y;
    info.hp = session->hp.load();
    info.max_hp = session->max_hp.load();
    info.exp = session->exp.load();
    info.level = session->level.load();
    info.visualId = 0;
    info.mp = session->mp.load();
    info.max_mp = session->max_mp.load();
    session->do_send(info.size, &info);

    // Stage 8: 인벤토리 + 장착 상태 전송 (신규는 빈 인벤, 기존은 복원된 상태)
    SendInventory(session);

    // Stage 9: 복원된 퀘스트 진행/완료 상태 동기화 (퀘스트 로그 영속)
    SendAllQuestUpdates(session);

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
        SendAddObject(other, session);
        SendAddObject(session, other);
    }

    SyncPlayerNpcView(session);

    // 위치/섹터/시야가 모두 확립된 뒤에만 게임플레이 패킷을 허용한다.
    // atomic store(release) → 이후 spawned=true를 본 스레드는 위의 모든 초기화도 관측한다.
    session->spawned.store(true);

    g_timer_manager.Schedule(client_id, TimerEventKind::HpRegen, HP_REGEN_INTERVAL_MS);
    g_timer_manager.Schedule(client_id, TimerEventKind::MpRegen, MP_REGEN_INTERVAL_MS);

#if VERBOSE_CLIENT_EVENTS
    auto name_ptr = atomic_load(&session->name);
    cout << "[Login] Client " << client_id << " (" << (exists ? "loaded" : "new")
         << ") as " << (name_ptr ? *name_ptr : std::string{}) << " at ("
         << session->x << ", " << session->y << ") hp=" << session->hp.load()
         << " lv=" << static_cast<int>(session->level.load()) << endl;
#endif
}

// DB 응답 디스패치 (worker_thread의 IO_DB_DONE 분기에서 호출).
void OnDbResponse(DbResponse& resp) {
    switch (resp.kind) {
    case DbReqKind::Load:
        if (!resp.ok) {
            cerr << "[Db] Load failed for client " << resp.client_id << endl;
            // DB 오류 시 클라이언트에 실패 응답 전송 후 연결 종료 (데이터 없이 입장 방지)
            {
                ClientMap::const_accessor a;
                if (g_clients.find(a, resp.client_id)) {
                    S2C_LoginResult fail;
                    fail.size = sizeof(fail);
                    fail.type = S2C_LOGIN_RESULT;
                    fail.success = false;
                    strcpy_s(fail.message, "Server error. Please reconnect.");
                    a->second->do_send(fail.size, &fail);
                    closesocket(a->second->socket);
                }
            }
            break;
        }
        OnPlayerSpawn(resp.client_id, resp.data, resp.exists);
        break;
    case DbReqKind::Save:
        if (!resp.ok) {
            cerr << "[Db] Save failed for client " << resp.client_id
                 << " (user=" << resp.data.username << ")" << endl;
        }
        break;
    }
}

// 30초마다 모든 활성 플레이어 상태를 EnqueueSave.
void PlayerOnAutoSave() {
    // 로그인한(이름 있는) 클라 id 스냅샷 — g_clients(tbb)를 직접 순회하면 동시 erase로 UB이므로
    // g_name_index(로그인~접속종료 동안만 존재)에서 id를 먼저 복사한 뒤 락 밖에서 find/save.
    vector<int> ids;
    {
        lock_guard<mutex> lk(g_name_index_mutex);
        ids.reserve(g_name_index.size());
        for (auto& kv : g_name_index) ids.push_back(kv.second);
    }
    for (int id : ids) {
        shared_ptr<Player> p;
        { ClientMap::const_accessor a; if (g_clients.find(a, id)) p = a->second; }
        if (!p) continue;  // 스냅샷 직후 접속종료된 경우
        auto name_ptr = atomic_load(&p->name);
        if (!name_ptr || name_ptr->empty()) continue;
        // 스폰 완료 전(로그인~OnPlayerSpawn 사이)에는 세션이 기본 스탯이므로 저장하면
        // 실제 DB 데이터를 덮어쓴다 — disconnect 저장과 동일하게 spawned로 게이팅.
        if (!p->spawned.load()) continue;
        g_db_worker.EnqueueSave(p->id, SnapshotPlayer(p));
    }
    // 다음 자동 저장 tick 재스케줄 (entity_id=0은 dummy)
    g_timer_manager.Schedule(0, TimerEventKind::PlayerAutoSave, PLAYER_AUTO_SAVE_INTERVAL_MS);
}

// 시스템 메시지를 S2C_CHAT_MESSAGE(object_id=0)로 특정 플레이어에게 전송
void SendSystemMessage(std::shared_ptr<Player> session, const std::string& msg) {
    S2C_ChatMessage cm;
    cm.size = sizeof(cm);
    cm.type = S2C_CHAT_MESSAGE;
    cm.object_id = 0;
    strncpy_s(cm.message, sizeof(cm.message), msg.c_str(), _TRUNCATE);
    cm.message[MAX_CHAT_MSG_LEN - 1] = '\0';
    session->do_send(cm.size, &cm);
}

// === Stage 9: 퀘스트 헬퍼 ===

// 퀘스트 상태 1건을 본인에게 전송 (수락/킬진행/완료/로그인복원 시).
void SendQuestUpdate(const std::shared_ptr<Player>& session, int quest_id,
                     int kill_count, int target_count, unsigned char state) {
    S2C_QuestUpdate u;
    u.size = sizeof(u);
    u.type = S2C_QUEST_UPDATE;
    u.quest_id = quest_id;
    u.kill_count = kill_count;
    u.target_count = target_count;
    u.state = state;
    session->do_send(u.size, &u);
}

// 로그인 직후: 보유한 모든 퀘스트 상태를 클라에 동기화 (퀘스트 로그 복원).
void SendAllQuestUpdates(const std::shared_ptr<Player>& session) {
    vector<Player::QuestProgress> snapshot;
    {
        lock_guard<mutex> lk(session->quest_lock);
        snapshot = session->quests;
    }
    for (const auto& q : snapshot) {
        const QuestDef* def = GetQuestDef(q.quest_id);
        int target = def ? def->target_count : q.kill_count;
        SendQuestUpdate(session, q.quest_id, q.kill_count, target, q.state);
    }
}

// 한 플레이어의 진행중 슬레이 퀘스트에 처치 1건 반영 + 변경 시 본인에게 동기화.
static void CreditQuestKill(const std::shared_ptr<Player>& p, const NPC& n) {
    struct Changed { int quest_id; int kill_count; int target; unsigned char state; };
    vector<Changed> changed;
    {
        lock_guard<mutex> lk(p->quest_lock);
        for (auto& q : p->quests) {
            if (q.state != 0) continue;  // active만
            const QuestDef* def = GetQuestDef(q.quest_id);
            if (!def) continue;
            if (q.kill_count >= def->target_count) continue;  // 이미 충족
            if (!QuestSpeciesMatches(def->target_species, n.name)) continue;
            q.kill_count++;
            if (q.kill_count > def->target_count) q.kill_count = def->target_count;
            changed.push_back({ q.quest_id, q.kill_count, def->target_count, q.state });
        }
    }
    for (const auto& c : changed) {
        SendQuestUpdate(p, c.quest_id, c.kill_count, c.target, c.state);
    }
}

// NPC 처치 시 호출 (전투/스킬 처치 3지점).
// 파티가 있으면 온라인 파티원 전원에게, 없으면 처치자 본인에게 킬 크레딧.
// (EXP 분배 GiveExpToKillerAndParty와 동일한 "멤버 수집 → 락 밖 처리" 패턴)
void OnNpcKilledForQuest(const std::shared_ptr<Player>& killer, const NPC& n) {
    if (g_quest_defs.empty()) return;

    int party_id_val = killer->party_id.load();
    if (party_id_val < 0) { CreditQuestKill(killer, n); return; }

    vector<int> member_ids;
    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_parties.find(party_id_val);
        if (it != g_parties.end()) member_ids = it->second->members;
    }

    vector<shared_ptr<Player>> online_members;
    for (int mid : member_ids) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, mid)) online_members.push_back(a->second);
    }
    if (online_members.empty()) { CreditQuestKill(killer, n); return; }

    for (auto& m : online_members) CreditQuestKill(m, n);
}

// === Stage 8: 바닥 아이템 헬퍼 ===
// (x,y) 주변 3x3 섹터 안 모든 플레이어에게 패킷 송신 (바닥 아이템 생성/제거 통지).
template <typename Pkt>
static void BroadcastToSectorPlayers(short x, short y, Pkt& pkt) {
    int sx = x / SECTOR_SIZE, sy = y / SECTOR_SIZE;
    vector<int> pids;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = sx + dx, ny = sy + dy;
            if (nx < 0 || nx >= NUM_SECTORS_X || ny < 0 || ny >= NUM_SECTORS_Y) continue;
            lock_guard<mutex> lock(g_sectors[ny][nx].m_lock);
            for (int pid : g_sectors[ny][nx].players) pids.push_back(pid);
        }
    }
    for (int pid : pids) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, pid)) a->second->do_send(pkt.size, &pkt);
    }
}

// session 시야 내 다른 플레이어(NPC 제외)에게 패킷 송신. include_self면 자신에게도.
// 공격 애니/스킬 이펙트/채팅/레벨업 등 "시야 내 플레이어 브로드캐스트" 공용 경로.
template <typename Pkt>
static void BroadcastToViewerPlayers(const std::shared_ptr<Player>& session, Pkt& pkt, bool include_self) {
    vector<int> pids;
    {
        lock_guard<mutex> lock(session->view_lock);
        for (int vid : session->view_list)
            if (!IsNpcId(vid)) pids.push_back(vid);
    }
    for (int vid : pids) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, vid)) a->second->do_send(pkt.size, &pkt);
    }
    if (include_self) session->do_send(pkt.size, &pkt);
}

// NPC 처치 시 드롭 판정 → 바닥 아이템 생성 + 주변 통지 + 만료 타이머 예약.
void SpawnNpcLoot(NpcType type, NpcMoveMode mode, int level, short x, short y) {
    if (g_item_defs.empty()) return;
    if (!RollShouldDrop(type, mode)) return;

    vector<int> items = RollDropItems(type, level);
    // 보스가 여러 개 떨어뜨릴 때 한 칸에 겹치지 않도록 약간 분산
    static const int OFF[3][2] = { {0,0}, {1,0}, {-1,0} };
    int idx = 0;
    for (int item_id : items) {
        const ItemDef* def = GetItemDef(item_id);
        if (!def) { ++idx; continue; }

        short dx = static_cast<short>(std::clamp(x + OFF[idx % 3][0], 0, WORLD_WIDTH - 1));
        short dy = static_cast<short>(std::clamp(y + OFF[idx % 3][1], 0, WORLD_HEIGHT - 1));
        int drop_id = g_next_drop_id.fetch_add(1);
        {
            lock_guard<mutex> lk(g_ground_mutex);
            g_ground_items[drop_id] = GroundItem{ item_id, 1, dx, dy };
        }

        S2C_ItemDrop pkt;
        pkt.size = sizeof(pkt);
        pkt.type = S2C_ITEM_DROP;
        pkt.drop_id = drop_id;
        pkt.item_id = item_id;
        pkt.x = dx;
        pkt.y = dy;
        BroadcastToSectorPlayers(dx, dy, pkt);

        g_timer_manager.Schedule(drop_id, TimerEventKind::GroundItemExpire, GROUND_ITEM_EXPIRE_MS);
        ++idx;
    }
}

// 바닥 아이템 만료 (60초). 아직 줍히지 않았으면 제거 + 주변 통지.
void OnGroundItemExpire(int drop_id) {
    GroundItem gi{};
    bool found = false;
    {
        lock_guard<mutex> lk(g_ground_mutex);
        auto it = g_ground_items.find(drop_id);
        if (it != g_ground_items.end()) { gi = it->second; g_ground_items.erase(it); found = true; }
    }
    if (!found) return;  // 이미 줍힘

    S2C_ItemRemove pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_ITEM_REMOVE;
    pkt.drop_id = drop_id;
    BroadcastToSectorPlayers(gi.x, gi.y, pkt);
}

// 인벤토리에 아이템 추가 (스택 가능하면 누적, 아니면 새 슬롯). 호출자가 inv_lock 보유.
// 반환: 전량 추가 성공 true / 슬롯 가득이라 일부라도 못 넣으면 false (이 경우 부분 추가될 수 있음).
static bool AddToInventoryLocked(Player& p, int item_id, int qty) {
    const ItemDef* def = GetItemDef(item_id);
    if (!def || qty <= 0) return false;

    if (def->stack_max > 1) {  // 소모품: 기존 슬롯에 누적
        for (auto& slot : p.inventory) {
            if (slot.first == item_id && slot.second < def->stack_max) {
                int add = std::min(qty, def->stack_max - slot.second);
                slot.second += add;
                qty -= add;
                if (qty <= 0) return true;
            }
        }
    }
    while (qty > 0) {
        if (static_cast<int>(p.inventory.size()) >= MAX_INVENTORY_SLOTS) return false;
        int add = (def->stack_max > 1) ? std::min(qty, def->stack_max) : 1;
        p.inventory.emplace_back(item_id, add);
        qty -= add;
    }
    return true;
}

// 인벤토리 + 장착 상태 전체 스냅샷을 본인에게 전송.
void SendInventory(const std::shared_ptr<Player>& session) {
    S2C_Inventory pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_INVENTORY;
    {
        lock_guard<mutex> lk(session->inv_lock);
        int n = std::min(static_cast<int>(session->inventory.size()), MAX_INVENTORY_SLOTS);
        pkt.count = static_cast<unsigned char>(n);
        for (int i = 0; i < MAX_INVENTORY_SLOTS; ++i) {
            if (i < n) { pkt.slots[i].item_id = session->inventory[i].first; pkt.slots[i].qty = session->inventory[i].second; }
            else       { pkt.slots[i].item_id = 0; pkt.slots[i].qty = 0; }
        }
    }
    pkt.equipped_weapon_id = session->equipped_weapon_id.load();
    pkt.equipped_armor_id  = session->equipped_armor_id.load();
    session->do_send(pkt.size, &pkt);
}

// 현재 HP/스탯을 "시야와 무관하게" 온라인 파티원에게만 S2C_StatusChange로 전송 (본인 제외).
// S2C_DAMAGE/DEATH/RESPAWN은 모두 시야 기반이라 시야 밖 파티원의 HP바가 갱신되지 않는다.
// 따라서 플레이어 HP가 변하는 모든 지점(피격/회복/부활)에서 호출해 파티 HP바를 동기화한다.
// 본인은 이미 S2C_DAMAGE 등으로 자기 HP를 받으므로 여기서 self 전송은 생략한다.
void BroadcastHpToParty(const std::shared_ptr<Player>& session) {
    int party_id_val = session->party_id.load();
    if (party_id_val < 0) return;
    vector<int> members;
    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_parties.find(party_id_val);
        if (it != g_parties.end()) members = it->second->members;
    }
    if (members.empty()) return;

    S2C_StatusChange sc;
    sc.size = sizeof(sc);
    sc.type = S2C_STATUS_CHANGE;
    sc.object_id = session->id;
    sc.hp = session->hp.load();
    sc.max_hp = session->max_hp.load();
    sc.exp = session->exp.load();
    sc.level = session->level.load();
    for (int mid : members) {
        if (mid == session->id) continue;
        ClientMap::const_accessor a;
        if (g_clients.find(a, mid)) a->second->do_send(sc.size, &sc);
    }
}

// 현재 스탯(hp/max_hp/exp/level)을 본인 + 파티원에게 S2C_StatusChange로 전송 (장착/회복 시 HUD 갱신).
void SendStatusChange(const std::shared_ptr<Player>& session) {
    S2C_StatusChange sc;
    sc.size = sizeof(sc);
    sc.type = S2C_STATUS_CHANGE;
    sc.object_id = session->id;
    sc.hp = session->hp.load();
    sc.max_hp = session->max_hp.load();
    sc.exp = session->exp.load();
    sc.level = session->level.load();
    session->do_send(sc.size, &sc);

    int party_id_val = session->party_id.load();
    if (party_id_val >= 0) {
        vector<int> members;
        { lock_guard<mutex> lk(g_party_mutex); auto it = g_parties.find(party_id_val); if (it != g_parties.end()) members = it->second->members; }
        for (int mid : members) {
            if (mid == session->id) continue;
            ClientMap::const_accessor a;
            if (g_clients.find(a, mid)) a->second->do_send(sc.size, &sc);
        }
    }
}

// EXP 추가 + 레벨업 체크 + S2C_LEVEL_UP 브로드캐스트 + S2C_StatusChange 자신·파티원 전송
void LevelUpPlayer(shared_ptr<Player> session, unsigned long long exp_gain) {
    session->exp.fetch_add(exp_gain);

    bool leveled_up = false;
    while (true) {
        unsigned char cur_lv = session->level.load();
        // 레벨업 임계 = 100 * level^2 (완만한 다항식). 구버전 100<<(level-1)는
        // 고레벨에서 시프트 폭 ≥ 64로 UB가 되므로 교체. (level≥1 보장 → 언더플로 없음)
        unsigned long long lvq = (cur_lv > 0) ? cur_lv : 1;
        unsigned long long need = 100ULL * lvq * lvq;
        unsigned long long cur_exp = session->exp.load();
        if (cur_exp < need) break;
        if (session->exp.compare_exchange_weak(cur_exp, cur_exp - need)) {
            // level도 CAS로 증가 — 파티 EXP 동시 처리 시 두 워커가 같은 cur_lv를 읽고
            // 각자 EXP를 소비한 뒤 level.fetch_add를 두 번 호출하는 race 방지.
            // CAS 실패 = 다른 스레드가 이미 이 레벨에서 레벨업 완료 → EXP 복원 후 재계산.
            unsigned char expected_lv = cur_lv;
            if (session->level.compare_exchange_strong(expected_lv, static_cast<unsigned char>(cur_lv + 1))) {
                int new_max = session->max_hp.fetch_add(20) + 20;
                session->hp.store(new_max);
                leveled_up = true;
            } else {
                session->exp.fetch_add(need);  // 소비 취소 후 다음 루프에서 새 레벨 기준으로 재계산
            }
        }
    }

    int pid = session->id;
    if (leveled_up) {
        S2C_LevelUp lvl;
        lvl.size = sizeof(lvl);
        lvl.type = S2C_LEVEL_UP;
        lvl.object_id = pid;
        lvl.new_level = session->level.load();
        lvl.new_max_hp = session->max_hp.load();

        BroadcastToViewerPlayers(session, lvl, true);
    }

    S2C_StatusChange sc;
    sc.size = sizeof(sc);
    sc.type = S2C_STATUS_CHANGE;
    sc.object_id = pid;
    sc.hp = session->hp.load();
    sc.max_hp = session->max_hp.load();
    sc.exp = session->exp.load();
    sc.level = session->level.load();

    // 레벨업이면 시야 내 모든 플레이어에게 브로드캐스트 (레벨·HP 표시 갱신 필요)
    // 그 외 EXP 획득·HP 변동은 자기 자신에게만 전송
    if (leveled_up) {
        BroadcastToViewerPlayers(session, sc, true);
    } else {
        session->do_send(sc.size, &sc);
    }

    // 파티원이 시야 밖에 있을 수 있으므로 파티 HP 바 갱신은 별도 전송
    int party_id_val = session->party_id.load();
    if (party_id_val >= 0) {
        vector<int> members;
        {
            lock_guard<mutex> lk(g_party_mutex);
            auto it = g_parties.find(party_id_val);
            if (it != g_parties.end()) members = it->second->members;
        }
        for (int mid : members) {
            if (mid == pid) continue;
            ClientMap::const_accessor a;
            if (g_clients.find(a, mid)) a->second->do_send(sc.size, &sc);
        }
    }
}

// NPC 처치 시 파티원 전체에 EXP 균등 분배. 파티 없으면 킬러 단독 획득.
void GiveExpToKillerAndParty(shared_ptr<Player> killer, unsigned long long exp_gain) {
    int party_id_val = killer->party_id.load();
    if (party_id_val < 0) {
        LevelUpPlayer(killer, exp_gain);
        return;
    }

    vector<int> member_ids;
    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_parties.find(party_id_val);
        if (it != g_parties.end()) member_ids = it->second->members;
    }

    vector<shared_ptr<Player>> online_members;
    for (int mid : member_ids) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, mid)) online_members.push_back(a->second);
    }
    if (online_members.empty()) {
        LevelUpPlayer(killer, exp_gain);
        return;
    }

    unsigned long long share = std::max(1ULL, exp_gain / online_members.size());
    for (auto& member : online_members) {
        LevelUpPlayer(member, share);
    }
}

// 처치한 NPC 1마리가 주는 EXP. 문서화된 다항식 모델로 통일 (GameConfig.h 주석과 일치).
//   일반: level^2 * BASE_EXP_MULTIPLIER  (Agro x2, Roaming x2)
//   보스: level^2 * BASE_EXP_MULTIPLIER * BOSS_EXP_MULTIPLIER
// (구버전은 지수형 1<<(level-1) + 보스 매직넘버 1<<30 → 중간레벨 폭주/오버플로 위험이라 교체)
unsigned long long ComputeNpcExp(const NPC& n) {
    unsigned long long lv = (n.level > 0) ? n.level : 1;
    unsigned long long base = lv * lv * (unsigned long long)BASE_EXP_MULTIPLIER;
    if (n.type == NpcType::Boss) return base * (unsigned long long)BOSS_EXP_MULTIPLIER;
    if (n.type == NpcType::Agro)             base *= 2;
    if (n.move_mode == NpcMoveMode::Roaming) base *= 2;
    return base;
}

// NPC에게 damage를 입히고 결과를 브로드캐스트. 막타면 사망 전체 처리(시야정리/섹터제거/
// S2C_DEATH/리스폰예약/EXP/루트/퀘스트)까지 수행. 기본공격/AoE/Line 등 모든 공격 경로 공용.
// per-NPC view_lock이 데미지 적용을 직렬화하므로, hp를 0으로 만든 막타 호출만 사망 경로를
// 정확히 1회 실행한다 (이중 EXP/루트/리스폰 방지).
// 반환: 이번 타격으로 NPC가 죽었으면 true.
bool DamageNpcAndReport(const std::shared_ptr<Player>& attacker, int nid, int damage) {
    const int client_id = attacker->id;
    NPC& n = GetNpc(nid);

    int new_hp = 0;
    short nx = 0, ny = 0;
    bool dealt = false;
    {
        lock_guard<mutex> lock(n.view_lock);
        if (n.hp > 0) {
            n.hp -= damage;
            if (n.hp < 0) n.hp = 0;
            new_hp = n.hp;
            nx = n.x;
            ny = n.y;
            dealt = true;
        }
    }
    if (!dealt) return false;

    // S2C_DAMAGE — NPC view_list의 모든 플레이어 + 공격자(중복 방지 위해 set 사용)
    S2C_Damage dmg;
    dmg.size = sizeof(dmg);
    dmg.type = S2C_DAMAGE;
    dmg.attacker_id = client_id;
    dmg.target_id = nid;
    dmg.damage = damage;
    dmg.new_hp = new_hp;
    dmg.target_x = nx;
    dmg.target_y = ny;

    unordered_set<int> dmg_viewers;
    {
        lock_guard<mutex> lock(n.view_lock);
        for (int vid : n.view_list) dmg_viewers.insert(vid);
    }
    dmg_viewers.insert(client_id);  // 공격자도 항상 받음
    for (int vid : dmg_viewers) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, vid)) a->second->do_send(dmg.size, &dmg);
    }

    if (new_hp != 0) return false;

    // === NPC 사망 처리: state=Dead + 시야 정리 + 섹터 제거 + 리스폰 예약 ===
    unordered_set<int> death_viewers;
    {
        lock_guard<mutex> lock(n.view_lock);
        for (int vid : n.view_list) death_viewers.insert(vid);
        n.view_list.clear();
        n.state = NpcFsmState::Dead;
        n.target_id = -1;
    }
    death_viewers.insert(client_id);  // 공격자가 시야 밖에서 죽일 일은 없지만 안전망

    // 시야 안 모든 플레이어의 view_list에서도 이 NPC 제거 (즉시 정리해두면 일관)
    for (int vid : death_viewers) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, vid)) {
            lock_guard<mutex> lock(a->second->view_lock);
            a->second->view_list.erase(nid);
        }
    }
    n.active.store(false);
    RemoveObjectFromSector(nid, nx, ny, false);

    // S2C_DEATH 브로드캐스트
    S2C_Death dpkt;
    dpkt.size = sizeof(dpkt);
    dpkt.type = S2C_DEATH;
    dpkt.object_id = nid;
    dpkt.death_x = nx;
    dpkt.death_y = ny;
    for (int vid : death_viewers) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, vid)) a->second->do_send(dpkt.size, &dpkt);
    }

    // 보스는 5분 리스폰, 일반 NPC는 30초
    int respawn_ms = (n.type == NpcType::Boss) ? BOSS_RESPAWN_MS : NPC_RESPAWN_MS;
    g_timer_manager.Schedule(nid, TimerEventKind::NpcRespawn, respawn_ms);

    // EXP: 파티원 균등 분배 (파티 없으면 킬러 단독) / 루트 드롭 / 퀘스트 처치 카운트
    GiveExpToKillerAndParty(attacker, ComputeNpcExp(n));
    SpawnNpcLoot(n.type, n.move_mode, n.level, nx, ny);  // Stage 8
    OnNpcKilledForQuest(attacker, n);                    // Stage 9
    return true;
}

// 파티 탈퇴 (disconnect 또는 C2S_PARTY_LEAVE 시 공통 경로).
// 혼자 남거나 리더가 나가면 파티 해산.
void PlayerLeaveParty(shared_ptr<Player> session) {
    // 펜딩 초대 정리 (초대자 또는 초대받은 쪽)
    {
        lock_guard<mutex> lk(g_party_mutex);
        g_pending_invites.erase(session->id);
        for (auto it = g_pending_invites.begin(); it != g_pending_invites.end(); ) {
            if (it->second.inviter_id == session->id) it = g_pending_invites.erase(it);
            else ++it;
        }
    }

    int party_id_val = session->party_id.exchange(-1);
    if (party_id_val < 0) return;

    int client_id = session->id;
    auto my_name = atomic_load(&session->name);

    vector<int> notify_list;
    bool disband = false;

    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_parties.find(party_id_val);
        if (it == g_parties.end()) return;
        Party* party = it->second.get();
        bool was_leader = (party->leader_id == client_id);

        party->members.erase(
            std::remove(party->members.begin(), party->members.end(), client_id),
            party->members.end());

        if (party->members.size() < 2) {
            // 혼자 남거나 비면 해산
            disband = true;
            notify_list = party->members;
            for (int mid : notify_list) {
                ClientMap::const_accessor a;
                if (g_clients.find(a, mid)) a->second->party_id.store(-1);
            }
            g_parties.erase(it);
        } else {
            notify_list = party->members;
            if (was_leader) party->leader_id = notify_list[0];
        }
    }

    S2C_PartyUpdate upd;
    upd.size = sizeof(upd);
    upd.type = S2C_PARTY_UPDATE;
    upd.event = disband ? 2 : 1;  // 2=disbanded, 1=left
    upd.member_id = client_id;
    strncpy_s(upd.member_name, sizeof(upd.member_name),
              my_name ? my_name->c_str() : "", _TRUNCATE);
    for (int mid : notify_list) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, mid)) a->second->do_send(upd.size, &upd);
    }
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

    // --- Stage 6: 장애물 맵 로드 (NPC 스폰보다 먼저: 스폰 시 IsBlocked 회피 가능) ---
    const char* obstacle_paths[] = {
        "data/obstacles.txt",
        "../../data/obstacles.txt",
        "../../../data/obstacles.txt",
    };
    int obstacle_rects = -1;
    for (const char* p : obstacle_paths) {
        obstacle_rects = Map::LoadObstacles(p);
        if (obstacle_rects >= 0) break;
    }
    if (obstacle_rects < 0) {
        cout << "[Map] obstacles.txt not found — world has no obstacles." << endl;
    }

    // --- Stage 8: 아이템 카탈로그 로드 (드롭/장착에서 사용) ---
    const char* item_paths[] = {
        "data/items.txt",
        "../../data/items.txt",
        "../../../data/items.txt",
    };
    int item_count = -1;
    for (const char* p : item_paths) {
        item_count = LoadItemDefs(p);
        if (item_count >= 0) break;
    }
    if (item_count >= 0) {
        cout << "[Item] Loaded " << item_count << " item defs." << endl;
    } else {
        cout << "[Item] items.txt not found — drops/equipment disabled." << endl;
    }

    // --- Stage 9: 퀘스트 카탈로그 로드 ---
    const char* quest_paths[] = {
        "data/quests.txt",
        "../../data/quests.txt",
        "../../../data/quests.txt",
    };
    int quest_count = -1;
    for (const char* p : quest_paths) {
        quest_count = LoadQuestDefs(p);
        if (quest_count >= 0) break;
    }
    if (quest_count >= 0) {
        cout << "[Quest] Loaded " << quest_count << " quest defs." << endl;
        // 퀘스트 보상 아이템이 카탈로그에 존재하는지 부팅 시 검증
        for (const auto& kv : g_quest_defs) {
            const QuestDef& qd = kv.second;
            if (qd.reward_item_id >= 0 && !GetItemDef(qd.reward_item_id)) {
                cout << "[Warn] Quest id=" << qd.id
                     << " reward_item_id=" << qd.reward_item_id
                     << " is not in items catalog — reward will be skipped." << endl;
            }
        }
    } else {
        cout << "[Quest] quests.txt not found — quests disabled." << endl;
    }

    // --- Stage 4: NPC 스폰 ---
    // 실행 디렉토리가 프로젝트 루트일 수도, x64/Release일 수도 있으므로 후보 경로를 순서대로 시도
    InitWorld(NUM_NPCS);
    const char* spawn_paths[] = {
        "data/npc_spawn.txt",
        "../../data/npc_spawn.txt",
        "../../../data/npc_spawn.txt",
    };
    int spawned = -1;
    for (const char* p : spawn_paths) {
        spawned = LoadNpcSpawnScript(p);
        if (spawned >= 0) break;
    }
    if (spawned > 0) {
        int fixed_hp = 0;
        for (int i = 0; i < spawned; ++i) {
            NPC& n = g_npcs[i];
            if (n.hp <= 0) { n.hp = 10; n.max_hp = 10; ++fixed_hp; }  // hp=0 NPC 보정
            UpdateObjectSector(n.id, -1, -1, n.x, n.y, false);
        }
        if (fixed_hp > 0)
            cout << "[Warn] " << fixed_hp << " NPC(s) had hp<=0 in spawn data — corrected to 10." << endl;
        cout << "[World] " << spawned << " NPCs placed into sectors." << endl;
    }
    else {
        cout << "[World] No NPCs spawned (script missing or empty)." << endl;
    }

    // --- Lua AI 초기화: 스크립트 경로 해석 → 부팅 검증 VM 로드 ---
    // [perf] 실제 AI 핫패스는 워커별 thread_local VM(GetWorkerLua)이 처리한다.
    // 여기서는 경로를 g_lua_script_path에 확정(워커가 재사용)하고, g_lua로 1회 로드 검증/로그만 한다.
    const char* lua_paths[] = {
        "data/npc_ai.lua",
        "../../data/npc_ai.lua",
        "../../../data/npc_ai.lua",
    };
    for (const char* p : lua_paths) {
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) { g_lua_script_path = p; break; }
    }

    bool lua_loaded = InitWorkerLuaVM(g_lua);  // 상수 노출 + DoFile (SetupLuaConstants 공유)
    if (!lua_loaded) {
        cout << "[Lua] Failed to load npc_ai.lua: " << g_lua.GetLastError() << endl;
        cout << "[Lua] NPC AI will be DISABLED (NpcTick will fail silently)." << endl;
    }
    else {
        cout << "[Lua] npc_ai.lua loaded (" << g_lua_script_path << "). per-worker VMs enabled. AGRO_RANGE=" << AGRO_DETECT_RANGE
             << " ROAM_RANGE=" << ROAM_AREA_RANGE
             << " TICK=" << NPC_TICK_INTERVAL_MS << "ms" << endl;
    }

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

    // --- Stage 6.3 + DB연동: DB 워커 시작 ---
    // 우선순위: data/db.ini의 enabled=1 + SQL Server(ODBC) 연결 성공 시 SQL Server 백엔드 사용.
    // 미설정/연결 실패 시 JSON 파일 백엔드로 자동 폴백 → 무DB 환경에서도 서버 항상 동작.
    // 응답 콜백: PostQueuedCompletionStatus로 IOCP에 IO_DB_DONE 이벤트 post.
    const char* parent_candidates[] = { "data", "../../data", "../../../data" };
    std::string data_dir = "data";
    std::string db_root = "data/players";
    for (const char* p : parent_candidates) {
        DWORD attr = GetFileAttributesA(p);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            data_dir = p;
            db_root = std::string(p) + "/players";
            break;
        }
    }

    // db.ini 파싱 (key=value, '#' 주석). 키: enabled / conn(ODBC 연결 문자열)
    bool db_enabled = false;
    std::string db_conn;
    {
        std::ifstream ini(data_dir + "/db.ini");
        std::string line;
        while (std::getline(ini, line)) {
            size_t s = line.find_first_not_of(" \t\r\n");
            if (s == std::string::npos || line[s] == '#') continue;
            size_t eq = line.find('=', s);
            if (eq == std::string::npos) continue;
            std::string key = line.substr(s, eq - s);
            size_t ke = key.find_last_not_of(" \t");
            key = (ke == std::string::npos) ? std::string() : key.substr(0, ke + 1);
            std::string val = line.substr(eq + 1);
            size_t vs = val.find_first_not_of(" \t");
            val = (vs == std::string::npos) ? std::string() : val.substr(vs);
            size_t ve = val.find_last_not_of(" \t\r\n");
            val = (ve == std::string::npos) ? std::string() : val.substr(0, ve + 1);
            if (key == "enabled")   db_enabled = (!val.empty() && val[0] == '1');
            else if (key == "conn") db_conn = val;
        }
    }

    std::unique_ptr<IDbBackend> db_backend;
    if (db_enabled && !db_conn.empty()) {
        auto mssql = std::make_unique<SqlServerOdbcBackend>(db_conn);
        if (mssql->Connected()) {
            db_backend = std::move(mssql);
            cout << "[Db] SQL Server(ODBC) connected." << endl;
        }
        else {
            cout << "[Db] SQL Server connect failed -> JSON fallback (root: " << db_root << ")" << endl;
        }
    }
    else {
        cout << "[Db] SQL Server disabled in db.ini -> JSON backend (root: " << db_root << ")" << endl;
    }
    if (!db_backend) {
        db_backend = std::make_unique<JsonFileBackend>(db_root);
    }

    g_db_worker.Start(
        std::move(db_backend),
        [](DbResponse&& resp) {
            auto* dov = g_db_pool.Acquire();
            dov->type = IO_DB_DONE;
            dov->response = std::move(resp);
            memset(&dov->overlapped, 0, sizeof(dov->overlapped));
            PostQueuedCompletionStatus(g_h_iocp, 0,
                static_cast<ULONG_PTR>(dov->response.client_id), &dov->overlapped);
        });

    // 주기적 자동 저장 타이머 첫 등록
    g_timer_manager.Schedule(0, TimerEventKind::PlayerAutoSave, PLAYER_AUTO_SAVE_INTERVAL_MS);

    vector<thread> worker_threads;
    // hardware_concurrency()는 코어 수를 못 구하면 0을 반환 → 그대로 쓰면 워커 0개로
    // 서버가 아무 IO도 처리 못 하고 멈춘다. 최소 2개 보장.
    int num_threads = thread::hardware_concurrency();
    if (num_threads < 2) num_threads = 2;
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back(worker_thread);
    }

    for (auto& t : worker_threads) t.join();

    g_db_worker.Stop();
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
                // Nagle 비활성화: 작은 move/이벤트 패킷이 ACK를 기다리며 묶이는 것을 막는다.
                // localhost(RTT~0)에선 무영향이지만 실제/원거리 RTT(VPN 등) 링크에선
                // Nagle+delayed-ACK가 패킷당 수십~수백 ms 지연을 유발한다.
                {
                    BOOL nodelay = TRUE;
                    setsockopt(c_socket, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                }
                int new_id = g_next_id++;
                // 플레이어 ID는 재사용 없이 단조 증가(IOCP 인플라이트 완료의 오라우팅 방지).
                // 누적 100만 접속 시 NPC_ID_START에 도달하면 IsNpcId()가 플레이어를 NPC로 오인 →
                // 조용한 상태 오염. 현실적으로 도달 불가하지만, 도달 시 안전하게 거절(방어).
                if (new_id >= NPC_ID_START) {
                    cout << "[Fatal] Player ID space exhausted (reached NPC ID range). Rejecting connection." << endl;
                    closesocket(c_socket);
                } else {
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

#if VERBOSE_CLIENT_EVENTS
                cout << "[Connect] Client Connected. ID: " << new_id << " (Total: " << g_clients.size() << ")" << endl;
#endif

                S2C_LoginResult res;
                res.size = sizeof(res);
                res.type = S2C_LOGIN_RESULT;
                res.success = true;
                strcpy_s(res.message, "Connected to MMO Server!");
                session->do_send(res.size, &res);

                session->do_recv();
                }  // new_id < NPC_ID_START
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
            TimerEventKind kind = tov->kind;
            int entity_id = static_cast<int>(completion_key);
            g_timer_pool.Release(tov);

            switch (kind) {
            case TimerEventKind::NpcMove:
                if (IsNpcId(entity_id)) NpcOnMove(entity_id);
                break;
            case TimerEventKind::NpcRespawn:
                if (IsNpcId(entity_id)) NpcOnRespawn(entity_id);
                break;
            case TimerEventKind::HpRegen:
                if (!IsNpcId(entity_id)) PlayerOnHpRegen(entity_id);
                break;
            case TimerEventKind::MpRegen:
                if (!IsNpcId(entity_id)) PlayerOnMpRegen(entity_id);
                break;
            case TimerEventKind::PlayerAutoSave:
                PlayerOnAutoSave();
                break;
            case TimerEventKind::GroundItemExpire:
                OnGroundItemExpire(entity_id);  // entity_id 자리에 drop_id 전달됨
                break;
            case TimerEventKind::PartyInviteExpire:
                OnPartyInviteExpire(entity_id); // entity_id = invitee client_id
                break;
            default:
                break;
            }
            continue;
        }

        // Stage 6.3: DB 완료 이벤트 (DbWorker → PostQueuedCompletionStatus)
        if (ov_ex->type == IO_DB_DONE) {
            auto* dov = reinterpret_cast<DbOverlapped*>(ov_ex);
            // OnDbResponse 안에서 OnPlayerSpawn 등이 호출됨 — 응답 데이터를 옮긴 뒤 풀로 회수
            DbResponse resp = std::move(dov->response);
            g_db_pool.Release(dov);
            OnDbResponse(resp);
            continue;
        }

        int client_id = static_cast<int>(completion_key);

        if (!result || bytes_transferred == 0) {
            if (ov_ex->type == IO_SEND) {
                // [perf 2A] 송신 실패 완료 — 풀 객체 회수(누수 방지) 후 일반 단절 정리로 진행.
                g_send_pools[ov_ex->pool_shard].Release(ov_ex);
            }
            shared_ptr<Player> disconnected;
            {
                ClientMap::const_accessor a;
                if (g_clients.find(a, client_id)) disconnected = a->second;
            }
            if (disconnected) {
#if VERBOSE_CLIENT_EVENTS
                cout << "[Disconnect] Client Disconnected. ID: " << client_id << endl;
#endif

                // Stage 6.3: 최종 상태를 DB에 저장 (이름이 비어있으면 LOGIN 전이므로 skip)
                {
                    auto name_ptr = atomic_load(&disconnected->name);
                    if (name_ptr && !name_ptr->empty()) {
                        // 저장은 스폰 완료(OnPlayerSpawn에서 DB Load 데이터 적재) 이후에만.
                        // 로그인~스폰 사이(또는 Load 실패로 강제 종료)에는 세션이 기본 스탯
                        // (hp=100/level=1/exp=0/빈 인벤)이라, 저장하면 실제 저장 데이터를
                        // 기본값으로 덮어써 진행 상황이 유실된다. spawned로 게이팅.
                        if (disconnected->spawned.load())
                            g_db_worker.EnqueueSave(client_id, SnapshotPlayer(disconnected));
                        // 이름 인덱스 제거는 스폰 여부와 무관 — HandleLogin에서 등록되므로
                        // 항상 제거해야 누수/재로그인 차단을 막는다 (동명이인의 다른 항목은 보존).
                        lock_guard<mutex> lk(g_name_index_mutex);
                        auto range = g_name_index.equal_range(*name_ptr);
                        for (auto it = range.first; it != range.second; ) {
                            if (it->second == client_id) it = g_name_index.erase(it);
                            else ++it;
                        }
                    }
                }

                // 파티 탈퇴 처리 (남은 파티원에게 알림 후 섹터/뷰 정리)
                PlayerLeaveParty(disconnected);

                RemoveObjectFromSector(client_id, disconnected->x, disconnected->y, true);

                // view_list 스냅샷 + 클리어 → 시야에 있던 다른 entity들에게 Remove 통보, 그쪽 view_list에서도 제거
                vector<int> viewers;
                {
                    lock_guard<mutex> lock(disconnected->view_lock);
                    viewers.assign(disconnected->view_list.begin(), disconnected->view_list.end());
                    disconnected->view_list.clear();
                }
                // Player와 NPC를 분리 처리. Player는 Remove 패킷까지 보내고, NPC는 view_list만 정리
                vector<shared_ptr<Player>> viewer_sessions;
                for (int id : viewers) {
                    if (IsNpcId(id)) {
                        NPC& n = GetNpc(id);
                        lock_guard<mutex> lk(n.view_lock);
                        n.view_list.erase(client_id);
                        // target_id도 즉시 해제 — 다음 tick까지 유령 타겟 방지
                        if (n.target_id == client_id) n.target_id = -1;
                        // NPC 비활성화는 다음 tick에서 view_list 비었음을 확인하고 스스로 수행
                    }
                    else {
                        ClientMap::const_accessor a;
                        if (g_clients.find(a, id)) viewer_sessions.push_back(a->second);
                    }
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

                    // 최소 2바이트(size + type) 미만은 비정상 패킷 — 연결 강제 종료
                    if (packet_size < 2) {
                        closesocket(session->socket);
                        break;
                    }

                    // 버퍼에 저장된 데이터가 패킷의 크기 이상이면 온전한 패킷 완성
                    if (session->packet_buffer.GetStoredSize() >= packet_size) {
                        // 0으로 초기화: size 필드를 줄여 보낸 조작 패킷이 구조체 뒤쪽(미수신 영역)에서
                        // 직전 패킷의 잔존 스택 데이터를 읽는 것을 방지(미초기화 읽기 UB 차단).
                        unsigned char packet_data[256] = { 0 }; // 임시 조립 버퍼 (프로토콜상 최대 255바이트)
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
            // [perf 2A] 코얼레싱 송신 완료: 부분전송 잔여를 큐 앞에 되돌리고 다음 배치 flush.
            int sent = static_cast<int>(bytes_transferred);
            int requested = static_cast<int>(ov_ex->wsa_buf.len);
            shared_ptr<Player> session;
            {
                ClientMap::const_accessor a;
                if (g_clients.find(a, client_id)) session = a->second;
            }
            if (session) {
                std::lock_guard<std::mutex> lk(session->send_lock);
                if (sent < requested) {
                    // 못 보낸 잔여(백프레셔)를 큐 앞에 되돌림 — 세션별 FIFO 순서 보존
                    session->send_queue.insert(session->send_queue.begin(),
                        ov_ex->buffer + sent, ov_ex->buffer + requested);
                }
                g_send_pools[ov_ex->pool_shard].Release(ov_ex);  // 원래 샤드로 반환
                if (!session->send_queue.empty()) session->flush_locked();  // 다음 배치
                else session->send_in_flight = false;
            } else {
                // 이미 단절된 세션 — 풀 객체만 회수 (버퍼가 세션과 독립이라 수명 안전)
                g_send_pools[ov_ex->pool_shard].Release(ov_ex);
            }
        }
    }
}

// === C2S 패킷 핸들러 (process_packet 디스패처가 호출) ===
// 각 핸들러는 session/client_id/ptr을 받아 해당 패킷 1종을 처리. 케이스 종료(break)는 return.

static void HandleLogin(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    // 이미 로그인된 세션에서 재처리 방지 (중복 C2S_LOGIN 패킷 또는 DB 요청 중복 방지)
    {
        auto existing = atomic_load(&session->name);
        if (existing && !existing->empty()) return;
    }

    C2S_Login* pkt = reinterpret_cast<C2S_Login*>(ptr);
    // null 종결 보장: username 배열이 null 없이 꽉 찼을 경우 OOB 방지
    std::string username(pkt->username, strnlen(pkt->username, MAX_NAME_LEN));
    if (username.empty()) return;

    // 동명 유저 중복 접속 거부 — JSON 백엔드 파일 충돌 및 파티 인덱스 오염 방지
    {
        lock_guard<mutex> lk(g_name_index_mutex);
        if (g_name_index.count(username) > 0) {
            S2C_LoginResult fail;
            fail.size = sizeof(fail);
            fail.type = S2C_LOGIN_RESULT;
            fail.success = false;
            strcpy_s(fail.message, "Username already in use.");
            session->do_send(fail.size, &fail);
            closesocket(session->socket);
            return;
        }
        // Stage 6.3: LOGIN 비동기화 — DB Load 큐잉. 응답 도착 시 OnPlayerSpawn에서 spawn/view 처리.
        g_name_index.emplace(username, client_id);
    }

    atomic_store(&session->name, make_shared<string>(username));
    g_db_worker.EnqueueLoad(client_id, std::move(username));
}

// 동시 피격(CAS 감소)과 경쟁해도 데미지를 삼키지 않는 HP 회복.
// 피격/리스폰/재생 경로가 모두 hp를 CAS로 다루므로(line 716/963/1238), 회복도 CAS 루프로
// 맞춰 lost-update를 막는다. 평범한 load-store 회복은 동시에 들어온 데미지를 덮어써
// '피격 무효화' 익스플로잇이 된다.
// 반환: 회복 후 HP(이미 풀피/사망이면 그 시점 값). 실제 회복이 일어났으면 healed=true.
static int HealPlayerHpAtomic(const std::shared_ptr<Player>& session, int amount, bool& healed) {
    healed = false;
    int max_hp = session->max_hp.load();
    if (amount <= 0) return session->hp.load();
    int observed = session->hp.load();
    while (true) {
        if (observed <= 0 || observed >= max_hp) return observed;  // 사망/풀피 → 회복 없음
        int next = std::min(max_hp, observed + amount);
        if (session->hp.compare_exchange_weak(observed, next)) { healed = true; return next; }
        // CAS 실패: observed가 최신값으로 갱신됨 → 루프 재평가
    }
}

// 이동 후 플레이어 주변 3×3(Chebyshev 1) 범위 내 힐링 아이템을 자동 픽업·즉시 사용.
// 인벤토리를 거치지 않고 HP를 직접 회복한다. HP가 이미 최대면 스킵.
static void TryAutoPickupHealItem(const shared_ptr<Player>& session, int client_id) {
    if (session->hp.load() >= session->max_hp.load()) return;

    short px = session->x, py = session->y;
    int best_drop_id = -1;
    GroundItem claimed{};

    {
        lock_guard<mutex> lk(g_ground_mutex);
        int best_dist = AUTO_HEAL_PICKUP_RANGE + 1;
        for (auto& kv : g_ground_items) {
            const ItemDef* def = GetItemDef(kv.second.item_id);
            if (!def || def->type != ItemType::Consumable) continue;
            int adx = abs(static_cast<int>(kv.second.x) - static_cast<int>(px));
            int ady = abs(static_cast<int>(kv.second.y) - static_cast<int>(py));
            int dist = max(adx, ady);  // Chebyshev distance → 3×3 영역
            if (dist <= AUTO_HEAL_PICKUP_RANGE && dist < best_dist) {
                best_dist = dist;
                best_drop_id = kv.first;
            }
        }
        if (best_drop_id >= 0) {
            claimed = g_ground_items[best_drop_id];
            g_ground_items.erase(best_drop_id);
        }
    }

    if (best_drop_id < 0) return;

    // 즉시 사용: 인벤토리 경유 없이 HP 직접 회복 (동시 피격과 경쟁해도 데미지 보존 — CAS)
    const ItemDef* def = GetItemDef(claimed.item_id);
    if (def) {
        bool healed;
        HealPlayerHpAtomic(session, def->value, healed);
    }

    // 주변 플레이어에게 아이템 제거 통지
    S2C_ItemRemove rm;
    rm.size = sizeof(rm);
    rm.type = S2C_ITEM_REMOVE;
    rm.drop_id = best_drop_id;
    BroadcastToSectorPlayers(claimed.x, claimed.y, rm);

    // 본인 HP 갱신 브로드캐스트
    SendStatusChange(session);
    SendSystemMessage(session, string("Auto-used ") + (def ? def->name : "potion") + "! HP restored.");
}

// C2S_MOVE / C2S_TELEPORT 공용. is_teleport면 쿨타임/거리 검증 생략(테스트용).
static void HandleMove(const shared_ptr<Player>& session, int client_id, unsigned char* ptr, bool is_teleport) {
    short req_x, req_y;
    int req_move_time;
    if (is_teleport) {
        C2S_Teleport* tp = reinterpret_cast<C2S_Teleport*>(ptr);
        req_x = tp->x;
        req_y = tp->y;
        req_move_time = 0;
    }
    else {
        C2S_Move* mp = reinterpret_cast<C2S_Move*>(ptr);
        req_x = mp->x;
        req_y = mp->y;
        req_move_time = mp->move_time;
    }

    // 이동 쿨타임 검증용 현재 시각 (ms)
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = session->last_move_ms.load();

    // 월드 경계 클램프 (클라가 보낸 좌표 신뢰 X) — req만 의존하므로 락 밖에서 선처리
    short new_x = req_x;
    short new_y = req_y;
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x >= WORLD_WIDTH)  new_x = WORLD_WIDTH - 1;
    if (new_y >= WORLD_HEIGHT) new_y = WORLD_HEIGHT - 1;

    // Stage 6: 장애물 칸으로의 이동/텔레포트는 거부 (no-op)
    if (Map::IsBlocked(new_x, new_y)) {
        return;
    }

    // === 위치 임계영역: move_lock으로 "현재 좌표 읽기 → 검증 → 섹터 이동 → 좌표 쓰기"를 원자화 ===
    // PlayerOnDeath(리스폰 텔레포트)가 다른 스레드에서 좌표를 바꾸는 것과 경쟁하지 않도록,
    // 현재 좌표를 반드시 락 안에서 읽어 그 좌표 기준으로 거리 검증한다. 그러면 사망 직후
    // stale 이동(사망 전 위치 기준 1칸)이 스폰 좌표를 덮어써 클라/서버가 어긋나는 문제를 막는다.
    short old_x, old_y;
    {
        lock_guard<mutex> mlk(session->move_lock);
        old_x = session->x;
        old_y = session->y;

        // 이동량 검증: Chebyshev distance (락 안의 최신 좌표 기준)
        int dx = std::abs(static_cast<int>(new_x) - static_cast<int>(old_x));
        int dy = std::abs(static_cast<int>(new_y) - static_cast<int>(old_y));
        int dist = std::max(dx, dy);
        if (dist == 0) return;  // 제자리 이동 패킷 무시 (RAII로 move_lock 해제)
        if (!is_teleport) {
            if (last != 0 && now_ms - last < PLAYER_MOVE_INTERVAL_MS) return;  // 쿨타임 미충족(치트/연사)
            if (dist > 1) return;  // 한 칸 초과 이동은 치트 — 사망 후 stale 이동도 여기서 거부됨
        }
        // 텔레포트 직후에도 last_move_ms를 갱신해 곧바로 한 칸 더 이동하는 것을 막음
        session->last_move_ms.store(now_ms);

        // 방향 갱신 — 공격 모션의 direction 결정용
        // dx/dy 중 더 큰 축 우선. 동률이면 dx 우선. 0=Down, 1=Left, 2=Right, 3=Up.
        int sdx = static_cast<int>(new_x) - static_cast<int>(old_x);
        int sdy = static_cast<int>(new_y) - static_cast<int>(old_y);
        if (std::abs(sdx) >= std::abs(sdy)) {
            if (sdx > 0)      session->direction.store(2); // Right
            else if (sdx < 0) session->direction.store(1); // Left
        }
        else {
            if (sdy > 0)      session->direction.store(0); // Down
            else if (sdy < 0) session->direction.store(3); // Up
        }

        // Sector 업데이트 + 좌표 확정 (move_lock이 sector 락보다 외곽 — 데드락 없음)
        UpdateObjectSector(client_id, old_x, old_y, new_x, new_y, true);
        session->x = new_x;
        session->y = new_y;
    }

    // 이동 패킷 브로드캐스팅
    S2C_MoveObject move_pkt;
    move_pkt.size = sizeof(move_pkt);
    move_pkt.type = S2C_MOVE_OBJECT;
    move_pkt.object_id = client_id;
    move_pkt.x = session->x;
    move_pkt.y = session->y;
    move_pkt.move_time = req_move_time;

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

    // 후보 + 기존 view_list의 player ID(NPC 제외)만 일괄 조회
    // NPC ID는 별도로 SyncPlayerNpcView가 처리 — 여기서 끌어오면 g_clients에 없어서 left 처리됨
    unordered_set<int> all_player_ids(candidate_ids.begin(), candidate_ids.end());
    {
        lock_guard<mutex> lock(session->view_lock);
        for (int id : session->view_list) {
            if (!IsNpcId(id)) all_player_ids.insert(id);
        }
    }
    unordered_map<int, shared_ptr<Player>> id_to_session;
    for (int id : all_player_ids) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, id)) id_to_session[id] = a->second;
    }

    // 시야 안의 player 집합 (새 view, player만)
    unordered_set<int> new_view;
    for (int id : candidate_ids) {
        auto it = id_to_session.find(id);
        if (it == id_to_session.end()) continue;
        if (IsInView(session->x, session->y, it->second->x, it->second->y)) {
            new_view.insert(id);
        }
    }

    // diff: player만 — NPC entry는 view_list에 그대로 둔다 (SyncPlayerNpcView가 책임)
    vector<int> entered_ids, left_ids, stayed_ids;
    {
        lock_guard<mutex> lock(session->view_lock);
        for (int id : new_view) {
            if (session->view_list.count(id)) stayed_ids.push_back(id);
            else entered_ids.push_back(id);
        }
        for (int id : session->view_list) {
            if (IsNpcId(id)) continue;
            if (!new_view.count(id)) left_ids.push_back(id);
        }
        for (int id : left_ids) session->view_list.erase(id);
        for (int id : entered_ids) session->view_list.insert(id);
    }

    // 새로 시야에 들어온 player: 상호 view_list 업데이트 + Add 전송
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
    // 시야에서 벗어난 player: 상호 view_list 정리 + Remove 전송
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
    // 유지된 player: Move 패킷 전송
    for (int id : stayed_ids) {
        auto it = id_to_session.find(id);
        if (it == id_to_session.end()) continue;
        it->second->do_send(move_pkt.size, &move_pkt);
    }
    // 본인에게도 이동 확인 패킷 (클라이언트의 자기 위치 갱신용)
    session->do_send(move_pkt.size, &move_pkt);

    // NPC 시야 동기화 (별도 경로 — player diff와 view_list 영역이 분리됨)
    SyncPlayerNpcView(session);

    // 이동 완료 후 주변 3×3 범위 힐링 아이템 자동 픽업·즉시 사용
    TryAutoPickupHealItem(session, client_id);
}

static void HandleAttack(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    // 쿨타임 (1초)
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last_atk = session->last_attack_ms.load();
    if (last_atk != 0 && now_ms - last_atk < ATTACK_INTERVAL_MS) return;
    session->last_attack_ms.store(now_ms);

    unsigned char dir = session->direction.load();
    short ax = session->x, ay = session->y;
    int lv = session->level.load();
    int damage = lv * BASE_DAMAGE_PER_LEVEL + session->atk_bonus.load();  // Stage 8: 무기 보너스

    // 1) S2C_ATTACK_ANIM 브로드캐스트 (시야 내 다른 플레이어 + 자기 자신)
    S2C_AttackAnim anim;
    anim.size = sizeof(anim);
    anim.type = S2C_ATTACK_ANIM;
    anim.object_id = client_id;
    anim.direction = dir;

    BroadcastToViewerPlayers(session, anim, true);

    // 2) 자기 타일 포함 5칸(자기 자신 + 상/하/좌/우)에서 NPC 찾기 → 데미지 + S2C_DAMAGE 브로드캐스트
    int adj[5][2] = { {ax, ay}, {ax, ay - 1}, {ax, ay + 1}, {ax - 1, ay}, {ax + 1, ay} };
    for (int i = 0; i < 5; ++i) {
        int tx = adj[i][0], ty = adj[i][1];
        if (tx < 0 || tx >= WORLD_WIDTH || ty < 0 || ty >= WORLD_HEIGHT) continue;
        int sx = tx / SECTOR_SIZE, sy = ty / SECTOR_SIZE;

        vector<int> npc_ids;
        {
            lock_guard<mutex> lock(g_sectors[sy][sx].m_lock);
            for (int nid : g_sectors[sy][sx].npcs) {
                NPC& nn = GetNpc(nid);
                if (nn.x == tx && nn.y == ty) npc_ids.push_back(nid);
            }
        }

        for (int nid : npc_ids) {
            DamageNpcAndReport(session, nid, damage);
        }
    }
}

static void HandleUseSkill(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_UseSkill* sp = reinterpret_cast<C2S_UseSkill*>(ptr);
    unsigned char skill_id = sp->skill_id;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 스킬별 MP 비용 결정 (알 수 없는 스킬은 거름). MP 부족하면 쿨타임 소모 전에 거절.
    int mp_cost = (skill_id == 1) ? SKILL_AOE_MP_COST
                : (skill_id == 2) ? SKILL_LINE_MP_COST
                : (skill_id == 3) ? SKILL_HEAL_MP_COST : -1;
    if (mp_cost < 0) return;
    if (session->mp.load() < mp_cost) { SendSystemMessage(session, "Not enough MP."); return; }

    // 스킬별 쿨타임 검증
    if (skill_id == 1) {
        long long last = session->last_skill1_ms.load();
        if (last != 0 && now_ms - last < SKILL_AOE_COOLDOWN_MS) return;
        session->last_skill1_ms.store(now_ms);
    } else if (skill_id == 2) {
        long long last = session->last_skill2_ms.load();
        if (last != 0 && now_ms - last < SKILL_LINE_COOLDOWN_MS) return;
        session->last_skill2_ms.store(now_ms);
    } else if (skill_id == 3) {
        long long last = session->last_skill3_ms.load();
        if (last != 0 && now_ms - last < SKILL_HEAL_COOLDOWN_MS) return;
        session->last_skill3_ms.store(now_ms);
    } else {
        return; // 알 수 없는 스킬 ID
    }

    // 쿨타임 통과 → MP CAS 차감 (음수 방지 + 재확인)
    int new_mp = 0;
    {
        int cur = session->mp.load();
        while (true) {
            if (cur < mp_cost) {
                SendSystemMessage(session, "Not enough MP.");
                return;
            }
            if (session->mp.compare_exchange_weak(cur, cur - mp_cost)) {
                new_mp = cur - mp_cost;
                break;
            }
        }
        S2C_MpChange mc;
        mc.size = sizeof(mc);
        mc.type = S2C_MP_CHANGE;
        mc.object_id = client_id;
        mc.mp = new_mp;
        mc.max_mp = session->max_mp.load();
        session->do_send(mc.size, &mc);
    }

    short cx = session->x, cy = session->y;
    int lv = session->level.load();

    // S2C_SKILL_EFFECT 브로드캐스트 (시야 내 플레이어 전원 + 자기 자신)
    S2C_SkillEffect sfx;
    sfx.size = sizeof(sfx);
    sfx.type = S2C_SKILL_EFFECT;
    sfx.object_id = client_id;
    sfx.skill_id  = skill_id;
    sfx.direction = session->direction.load();
    sfx.x = cx; sfx.y = cy;

    BroadcastToViewerPlayers(session, sfx, true);

    // --- 스킬 1: AoE — 반경 SKILL_AOE_RANGE 이내 모든 NPC에 데미지 ---
    if (skill_id == 1) {
        int damage = lv * SKILL_AOE_DAMAGE_PER_LEVEL + session->atk_bonus.load();  // Stage 8: 무기 보너스
        int r = SKILL_AOE_RANGE;

        // 영향권 섹터 열거
        int sx0 = max(0, (cx - r) / SECTOR_SIZE);
        int sy0 = max(0, (cy - r) / SECTOR_SIZE);
        int sx1 = min(NUM_SECTORS_X - 1, (cx + r) / SECTOR_SIZE);
        int sy1 = min(NUM_SECTORS_Y - 1, (cy + r) / SECTOR_SIZE);

        for (int sy = sy0; sy <= sy1; ++sy) {
            for (int sxx = sx0; sxx <= sx1; ++sxx) {
                vector<int> npc_ids;
                {
                    lock_guard<mutex> slk(g_sectors[sy][sxx].m_lock);
                    for (int nid : g_sectors[sy][sxx].npcs) npc_ids.push_back(nid);
                }
                for (int nid : npc_ids) {
                    NPC& n = GetNpc(nid);
                    // chebyshev 거리 체크
                    if (abs(n.x - cx) > r || abs(n.y - cy) > r) continue;
                    DamageNpcAndReport(session, nid, damage);
                }
            }
        }
    }
    // --- 스킬 2: Line — 현재 방향 직선 SKILL_LINE_RANGE칸의 모든 NPC에 데미지 ---
    else if (skill_id == 2) {
        int damage = lv * SKILL_LINE_DAMAGE_PER_LEVEL + session->atk_bonus.load();  // Stage 8: 무기 보너스
        unsigned char dir = session->direction.load();
        // dir: 0=Down(y+1), 1=Left(x-1), 2=Right(x+1), 3=Up(y-1)
        int ddx = 0, ddy = 0;
        switch (dir) { case 0: ddy = 1; break; case 1: ddx = -1; break; case 2: ddx = 1; break; case 3: ddy = -1; break; }

        for (int step = 0; step <= SKILL_LINE_RANGE; ++step) {  // step 0 = 시전자 자기 타일 포함
            int tx = cx + ddx * step;
            int ty = cy + ddy * step;
            if (tx < 0 || tx >= WORLD_WIDTH || ty < 0 || ty >= WORLD_HEIGHT) break;
            int sxx = tx / SECTOR_SIZE, sy = ty / SECTOR_SIZE;

            vector<int> npc_ids;
            { lock_guard<mutex> slk(g_sectors[sy][sxx].m_lock); for (int nid : g_sectors[sy][sxx].npcs) { NPC& n = GetNpc(nid); if (n.x == tx && n.y == ty) npc_ids.push_back(nid); } }

            for (int nid : npc_ids) {
                DamageNpcAndReport(session, nid, damage);
            }
        }
    }
    // --- 스킬 3: Heal — 자신 HP를 max_hp의 30% 회복 ---
    else if (skill_id == 3) {
        int max_hp = session->max_hp.load();
        bool healed;
        int new_hp = HealPlayerHpAtomic(session, max_hp * SKILL_HEAL_PERCENT / 100, healed);

        S2C_StatusChange sc; sc.size = sizeof(sc); sc.type = S2C_STATUS_CHANGE;
        sc.object_id = client_id; sc.hp = new_hp; sc.max_hp = max_hp;
        sc.exp = session->exp.load(); sc.level = session->level.load();
        session->do_send(sc.size, &sc);
    }
}

static void HandleChat(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_Chat* p = reinterpret_cast<C2S_Chat*>(ptr);

    // 안전 복사 + null 종결 보장
    S2C_ChatMessage msg;
    msg.size = sizeof(msg);
    msg.type = S2C_CHAT_MESSAGE;
    msg.object_id = client_id;
    strncpy_s(msg.message, sizeof(msg.message), p->message, _TRUNCATE);
    msg.message[MAX_CHAT_MSG_LEN - 1] = '\0';

    // 자기 자신 + 시야 내 다른 플레이어에게 송신 (NPC는 채팅 안 받음)
    BroadcastToViewerPlayers(session, msg, true);
}

// 파티 초대 30초 타임아웃. 아직 pending이면 취소하고 양쪽에 알림.
void OnPartyInviteExpire(int invitee_id) {
    int inviter_id = -1;
    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_pending_invites.find(invitee_id);
        if (it == g_pending_invites.end()) return;  // 이미 수락/거절됨
        // 묵은 타이머 보호: 현재 시각이 이 초대의 만료시각 이전이면, 이 자리는 이전 초대가
        // 해소된 뒤 들어온 더 새 초대다. 이 타이머는 그 이전(이미 해소된) 초대의 것이므로
        // 무시한다 — 새 초대는 자신의 타이머가 정시에 만료시킨다.
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms < it->second.expire_at_ms) return;
        inviter_id = it->second.inviter_id;
        g_pending_invites.erase(it);
    }
    // 초대한 쪽에게 타임아웃 알림
    if (inviter_id >= 0) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, inviter_id))
            SendSystemMessage(a->second, "Party invite expired (no response).");
    }
    // 초대 받은 쪽에게도 알림 (아직 접속 중이면)
    {
        ClientMap::const_accessor a;
        if (g_clients.find(a, invitee_id))
            SendSystemMessage(a->second, "Party invite has expired.");
    }
}

static void HandlePartyInvite(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_PartyInvite* p = reinterpret_cast<C2S_PartyInvite*>(ptr);
    // null 종결 보장: target_name 배열이 null 없이 꽉 찼을 경우 OOB 읽기 방지 (HandleLogin과 동일 패턴)
    string target_name(p->target_name, strnlen(p->target_name, MAX_NAME_LEN));

    // 내 파티가 꽉 찼으면 거부
    int my_party = session->party_id.load();
    if (my_party >= 0) {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_parties.find(my_party);
        if (it != g_parties.end() && (int)it->second->members.size() >= MAX_PARTY_SIZE) {
            SendSystemMessage(session, "Party is full.");
            return;
        }
    }

    // 이름으로 대상 찾기 — g_name_index O(1) 조회 (g_clients 직접 순회는 동시 erase로 UB).
    // 동명이인 대비 equal_range 순회 + 현재 이름 재확인. 자기 자신은 제외.
    shared_ptr<Player> target;
    {
        lock_guard<mutex> lk(g_name_index_mutex);
        auto range = g_name_index.equal_range(target_name);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == client_id) continue;
            ClientMap::const_accessor a;
            if (!g_clients.find(a, it->second)) continue;
            auto nptr = atomic_load(&a->second->name);
            if (nptr && *nptr == target_name) { target = a->second; break; }
        }
    }
    if (!target) { SendSystemMessage(session, "Player not found."); return; }
    if (target->party_id.load() >= 0) {
        SendSystemMessage(session, target_name + " is already in a party.");
        return;
    }
    // 예약 타이머의 만료 시각과 동일 기준(steady_clock)으로 expire_at_ms 기록 —
    // OnPartyInviteExpire가 묵은 타이머를 구분하는 데 사용.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        lock_guard<mutex> lk(g_party_mutex);
        if (g_pending_invites.count(target->id)) {
            SendSystemMessage(session, target_name + " already has a pending invite.");
            return;
        }
        g_pending_invites[target->id] = PendingInvite{ client_id, now_ms + PARTY_INVITE_TIMEOUT_MS };
    }
    // 30초 내 응답 없으면 자동 취소 (entity_id 자리에 invitee client_id 사용)
    g_timer_manager.Schedule(target->id, TimerEventKind::PartyInviteExpire, PARTY_INVITE_TIMEOUT_MS);

    auto my_name = atomic_load(&session->name);
    S2C_PartyInvited inv;
    inv.size = sizeof(inv);
    inv.type = S2C_PARTY_INVITED;
    inv.inviter_id = client_id;
    strncpy_s(inv.inviter_name, sizeof(inv.inviter_name),
              my_name ? my_name->c_str() : "", _TRUNCATE);
    target->do_send(inv.size, &inv);
    SendSystemMessage(session, "Invited " + target_name + " to party.");
}

static void HandlePartyAccept(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    // 이미 파티에 속해 있으면 수락 불가 — 그 사이 다른 초대를 먼저 수락한 경우
    if (session->party_id.load() >= 0) {
        SendSystemMessage(session, "You are already in a party.");
        return;
    }
    int inviter_id;
    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_pending_invites.find(client_id);
        if (it == g_pending_invites.end()) {
            SendSystemMessage(session, "No pending party invite.");
            return;
        }
        inviter_id = it->second.inviter_id;
        g_pending_invites.erase(it);
    }
    shared_ptr<Player> inviter;
    {
        ClientMap::const_accessor a;
        if (g_clients.find(a, inviter_id)) inviter = a->second;
    }
    if (!inviter) { SendSystemMessage(session, "Inviter has disconnected."); return; }

    auto my_name   = atomic_load(&session->name);
    int inviter_party = inviter->party_id.load();
    int new_party_id;
    vector<int> existing_members;

    {
        lock_guard<mutex> lk(g_party_mutex);
        if (inviter_party >= 0) {
            auto it = g_parties.find(inviter_party);
            if (it == g_parties.end() || (int)it->second->members.size() >= MAX_PARTY_SIZE) {
                SendSystemMessage(session, "Party is full.");
                return;
            }
            new_party_id = inviter_party;
            existing_members = it->second->members;
            it->second->members.push_back(client_id);
        } else {
            new_party_id = g_next_party_id++;
            auto party = make_shared<Party>();
            party->id = new_party_id;
            party->leader_id = inviter_id;
            party->members.push_back(inviter_id);
            party->members.push_back(client_id);
            g_parties[new_party_id] = party;
            existing_members.push_back(inviter_id);
        }
    }
    session->party_id.store(new_party_id);
    if (inviter_party < 0) inviter->party_id.store(new_party_id);

    // 기존 멤버들에게 신입 알림
    S2C_PartyUpdate upd;
    upd.size = sizeof(upd);
    upd.type = S2C_PARTY_UPDATE;
    upd.event = 0;  // joined
    upd.member_id = client_id;
    strncpy_s(upd.member_name, sizeof(upd.member_name),
              my_name ? my_name->c_str() : "", _TRUNCATE);
    for (int mid : existing_members) {
        ClientMap::const_accessor a;
        if (g_clients.find(a, mid)) a->second->do_send(upd.size, &upd);
    }
    // 신입에게 기존 멤버 목록 전송 (각 기존 멤버마다 JOINED 패킷 1개)
    for (int mid : existing_members) {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, mid)) continue;
        S2C_PartyUpdate info;
        info.size = sizeof(info);
        info.type = S2C_PARTY_UPDATE;
        info.event = 0;
        info.member_id = mid;
        auto mname = atomic_load(&a->second->name);
        strncpy_s(info.member_name, sizeof(info.member_name),
                  mname ? mname->c_str() : "", _TRUNCATE);
        session->do_send(info.size, &info);
    }
    // 파티 HP 상태 동기화: 신입 → 기존 멤버, 기존 멤버 → 신입
    {
        S2C_StatusChange my_sc;
        my_sc.size = sizeof(my_sc); my_sc.type = S2C_STATUS_CHANGE;
        my_sc.object_id = client_id;
        my_sc.hp = session->hp.load(); my_sc.max_hp = session->max_hp.load();
        my_sc.exp = session->exp.load(); my_sc.level = session->level.load();
        for (int mid : existing_members) {
            ClientMap::const_accessor a;
            if (g_clients.find(a, mid)) {
                a->second->do_send(my_sc.size, &my_sc);
                // 기존 멤버의 HP도 신입에게 전송
                S2C_StatusChange mem_sc;
                mem_sc.size = sizeof(mem_sc); mem_sc.type = S2C_STATUS_CHANGE;
                mem_sc.object_id = mid;
                mem_sc.hp = a->second->hp.load(); mem_sc.max_hp = a->second->max_hp.load();
                mem_sc.exp = a->second->exp.load(); mem_sc.level = a->second->level.load();
                session->do_send(mem_sc.size, &mem_sc);
            }
        }
    }
    SendSystemMessage(session, "Joined party!");
}

static void HandlePartyReject(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    int inviter_id;
    {
        lock_guard<mutex> lk(g_party_mutex);
        auto it = g_pending_invites.find(client_id);
        if (it == g_pending_invites.end()) return;
        inviter_id = it->second.inviter_id;
        g_pending_invites.erase(it);
    }
    auto my_name = atomic_load(&session->name);
    ClientMap::const_accessor a;
    if (g_clients.find(a, inviter_id)) {
        SendSystemMessage(a->second,
            string(my_name ? my_name->c_str() : "Unknown") + " rejected your invite.");
    }
    SendSystemMessage(session, "Rejected party invite.");
}

static void HandlePartyLeave(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    PlayerLeaveParty(session);
    SendSystemMessage(session, "You left the party.");
}

static void HandlePickup(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    short px = session->x, py = session->y;
    // 1) 가장 가까운 바닥 아이템 claim (제거) — 다른 worker와의 이중 줍기 방지
    int drop_id = -1;
    GroundItem claimed{};
    {
        lock_guard<mutex> lk(g_ground_mutex);
        int best_dist = ITEM_PICKUP_RANGE + 1;  // 이 값 미만만 채택
        for (auto& kv : g_ground_items) {
            int adx = (kv.second.x > px) ? kv.second.x - px : px - kv.second.x;
            int ady = (kv.second.y > py) ? kv.second.y - py : py - kv.second.y;
            int dist = (adx > ady) ? adx : ady;  // Chebyshev — TryAutoPickupHealItem과 일치
            if (dist <= ITEM_PICKUP_RANGE && dist < best_dist) { best_dist = dist; drop_id = kv.first; }
        }
        if (drop_id >= 0) { claimed = g_ground_items[drop_id]; g_ground_items.erase(drop_id); }
    }
    if (drop_id < 0) { SendSystemMessage(session, "No item nearby."); return; }

    // 2) 인벤토리에 추가
    bool added;
    { lock_guard<mutex> lk(session->inv_lock); added = AddToInventoryLocked(*session, claimed.item_id, claimed.count); }
    if (!added) {
        // 인벤 가득 → 새 drop_id로 바닥에 되돌림 + 새 만료 타이머 등록
        // (원래 drop_id의 타이머는 이미 진행 중이므로 새 ID를 발급해 중복 만료 방지)
        // 먼저 원래 drop_id의 제거를 브로드캐스트해야 한다 — 안 그러면 클라가 원래
        // 스프라이트를 계속 그려 새 스프라이트와 한 칸에 겹친 유령 아이템이 남는다
        // (원래 id는 맵에서 이미 지워져 OnGroundItemExpire가 제거 패킷을 보내지 않음).
        S2C_ItemRemove oldrm;
        oldrm.size = sizeof(oldrm); oldrm.type = S2C_ITEM_REMOVE; oldrm.drop_id = drop_id;
        BroadcastToSectorPlayers(claimed.x, claimed.y, oldrm);

        int new_drop_id = g_next_drop_id.fetch_add(1);
        {
            lock_guard<mutex> lk(g_ground_mutex);
            g_ground_items[new_drop_id] = claimed;
        }
        S2C_ItemDrop redrop;
        redrop.size = sizeof(redrop); redrop.type = S2C_ITEM_DROP;
        redrop.drop_id = new_drop_id; redrop.item_id = claimed.item_id;
        redrop.x = claimed.x; redrop.y = claimed.y;
        BroadcastToSectorPlayers(claimed.x, claimed.y, redrop);
        g_timer_manager.Schedule(new_drop_id, TimerEventKind::GroundItemExpire, GROUND_ITEM_EXPIRE_MS);
        SendSystemMessage(session, "Inventory full.");
        return;
    }

    // 3) 성공: 본인 인벤 갱신 + 주변에 제거 통지
    SendInventory(session);
    S2C_ItemRemove rm; rm.size = sizeof(rm); rm.type = S2C_ITEM_REMOVE; rm.drop_id = drop_id;
    BroadcastToSectorPlayers(claimed.x, claimed.y, rm);
    const ItemDef* def = GetItemDef(claimed.item_id);
    SendSystemMessage(session, std::string("Picked up ") + (def ? def->name : "item") + ".");
}

static void HandleUseItem(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_UseItem* p = reinterpret_cast<C2S_UseItem*>(ptr);
    int slot = p->slot;
    bool consumed = false;
    bool full_hp = false;
    {
        lock_guard<mutex> lk(session->inv_lock);
        if (slot < 0 || slot >= static_cast<int>(session->inventory.size())) return;
        const ItemDef* def = GetItemDef(session->inventory[slot].first);
        if (!def || def->type != ItemType::Consumable) return;

        int max_hp = session->max_hp.load();
        if (session->hp.load() >= max_hp) { full_hp = true; }
        else {
            bool healed;
            HealPlayerHpAtomic(session, def->value, healed);
            if (healed) {
                if (--session->inventory[slot].second <= 0)
                    session->inventory.erase(session->inventory.begin() + slot);
                consumed = true;
            } else {
                full_hp = true;  // 경쟁 중 풀피/사망 → 포션 소비 안 함
            }
        }
    }
    if (full_hp) { SendSystemMessage(session, "HP already full."); return; }
    if (consumed) { SendInventory(session); SendStatusChange(session); }
}

static void HandleEquipItem(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_EquipItem* p = reinterpret_cast<C2S_EquipItem*>(ptr);
    int slot = p->slot;
    bool changed = false;
    {
        lock_guard<mutex> lk(session->inv_lock);
        if (slot < 0 || slot >= static_cast<int>(session->inventory.size())) return;
        int item_id = session->inventory[slot].first;
        const ItemDef* def = GetItemDef(item_id);
        if (!def || (def->type != ItemType::Weapon && def->type != ItemType::Armor)) return;

        // 새 장착품을 인벤에서 제거(-1슬롯) → 기존 장착품 반환(+1슬롯)이라 절대 오버플로 없음
        session->inventory.erase(session->inventory.begin() + slot);
        if (def->type == ItemType::Weapon) {
            int old = session->equipped_weapon_id.exchange(item_id);
            session->atk_bonus.store(def->value);
            if (old >= 0) AddToInventoryLocked(*session, old, 1);
        } else {  // Armor
            int old = session->equipped_armor_id.exchange(item_id);
            const ItemDef* od = (old >= 0) ? GetItemDef(old) : nullptr;
            int oldbonus = od ? od->value : 0;
            int newmax = session->max_hp.load() - oldbonus + def->value;
            if (newmax < 1) newmax = 1;
            session->max_hp.store(newmax);
            if (session->hp.load() > newmax) session->hp.store(newmax);
            if (old >= 0) AddToInventoryLocked(*session, old, 1);
        }
        changed = true;
    }
    if (changed) { SendInventory(session); SendStatusChange(session); }
}

static void HandleUnequipItem(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_UnequipItem* p = reinterpret_cast<C2S_UnequipItem*>(ptr);
    int which = p->which;  // 0=weapon, 1=armor
    if (which > 1) return;  // 유효하지 않은 슬롯 타입 — 조작된 패킷 방어
    bool changed = false;
    {
        lock_guard<mutex> lk(session->inv_lock);
        if (static_cast<int>(session->inventory.size()) >= MAX_INVENTORY_SLOTS) {
            // 인벤 가득 → 해제 불가
        } else if (which == 0) {
            int old = session->equipped_weapon_id.exchange(-1);
            if (old >= 0) { session->atk_bonus.store(0); AddToInventoryLocked(*session, old, 1); changed = true; }
        } else if (which == 1) {
            int old = session->equipped_armor_id.exchange(-1);
            if (old >= 0) {
                const ItemDef* od = GetItemDef(old);
                int bonus = od ? od->value : 0;
                int newmax = session->max_hp.load() - bonus;
                if (newmax < 1) newmax = 1;
                session->max_hp.store(newmax);
                if (session->hp.load() > newmax) session->hp.store(newmax);
                AddToInventoryLocked(*session, old, 1);
                changed = true;
            }
        }
    }
    if (changed) { SendInventory(session); SendStatusChange(session); }
    else SendSystemMessage(session, "Cannot unequip (inventory full or nothing equipped).");
}

// 인벤 슬롯 아이템을 발밑 바닥에 버림. 슬롯 전체 수량을 바닥 아이템 1개(count=qty)로 생성 →
// 다시 줍거나(G) 60초 후 자동 소멸. 줍기/루트와 동일한 ground-item 시스템 재사용.
static void HandleDropItem(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_DropItem* p = reinterpret_cast<C2S_DropItem*>(ptr);
    int slot = p->slot;

    int item_id = -1, qty = 0;
    {
        lock_guard<mutex> lk(session->inv_lock);
        if (slot < 0 || slot >= static_cast<int>(session->inventory.size())) return;
        item_id = session->inventory[slot].first;
        qty     = session->inventory[slot].second;
        session->inventory.erase(session->inventory.begin() + slot);
    }
    const ItemDef* def = GetItemDef(item_id);
    if (!def || qty <= 0) { SendInventory(session); return; }

    short dx = session->x, dy = session->y;
    int drop_id = g_next_drop_id.fetch_add(1);
    {
        lock_guard<mutex> lk(g_ground_mutex);
        g_ground_items[drop_id] = GroundItem{ item_id, qty, dx, dy };
    }

    S2C_ItemDrop pkt;
    pkt.size = sizeof(pkt);
    pkt.type = S2C_ITEM_DROP;
    pkt.drop_id = drop_id;
    pkt.item_id = item_id;
    pkt.x = dx;
    pkt.y = dy;
    BroadcastToSectorPlayers(dx, dy, pkt);
    g_timer_manager.Schedule(drop_id, TimerEventKind::GroundItemExpire, GROUND_ITEM_EXPIRE_MS);

    SendInventory(session);
    SendSystemMessage(session, std::string("Dropped ") + def->name +
                      (qty > 1 ? " x" + std::to_string(qty) : "") + ".");
}

static void HandleQuestInteract(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_QuestInteract* p = reinterpret_cast<C2S_QuestInteract*>(ptr);
    if (p->npc_index != 0) return;  // MVP: 장로(0)만 퀘스트 제공

    // 근접성 검증 — 클라 좌표 불신, 서버 좌표로 chebyshev 거리 체크
    int gdx = abs(static_cast<int>(session->x) - static_cast<int>(QUEST_GIVER_X));
    int gdy = abs(static_cast<int>(session->y) - static_cast<int>(QUEST_GIVER_Y));
    if (max(gdx, gdy) > QUEST_INTERACT_RANGE) {
        SendSystemMessage(session, "You are too far from the Elder.");
        return;
    }

    // 보유 퀘스트 스냅샷
    vector<Player::QuestProgress> snap;
    { lock_guard<mutex> lk(session->quest_lock); snap = session->quests; }
    auto find_q = [&](int qid) -> const Player::QuestProgress* {
        for (auto& q : snap) if (q.quest_id == qid) return &q;
        return nullptr;
    };

    int dlg_quest = -1;
    unsigned char kind = 3;  // None

    // 1) 보상 수령 가능 (active + 카운트 충족) — 가장 낮은 id
    for (const auto& q : snap) {
        if (q.state != 0) continue;
        const QuestDef* def = GetQuestDef(q.quest_id);
        if (def && q.kill_count >= def->target_count) {
            if (dlg_quest == -1 || q.quest_id < dlg_quest) dlg_quest = q.quest_id;
        }
    }
    if (dlg_quest != -1) { kind = 2; }  // ReadyTurnIn
    else {
        // 2) 진행 중 (미충족) — 가장 낮은 id
        for (const auto& q : snap) {
            if (q.state != 0) continue;
            if (dlg_quest == -1 || q.quest_id < dlg_quest) dlg_quest = q.quest_id;
        }
        if (dlg_quest != -1) { kind = 1; }  // InProgress
        else {
            // 3) offer 가능 (giver=0, 미보유, 선행 완료) — 가장 낮은 id
            for (const auto& kv : g_quest_defs) {
                const QuestDef& def = kv.second;
                if (def.giver_npc != 0) continue;
                if (find_q(def.id)) continue;  // 이미 보유(진행 또는 완료)
                if (def.prereq_id >= 0) {
                    const Player::QuestProgress* pr = find_q(def.prereq_id);
                    if (!pr || pr->state != 1) continue;  // 선행 미완료
                }
                if (dlg_quest == -1 || def.id < dlg_quest) dlg_quest = def.id;
            }
            if (dlg_quest != -1) kind = 0;  // Offer
        }
    }

    S2C_QuestDialogue dlg;
    dlg.size = sizeof(dlg);
    dlg.type = S2C_QUEST_DIALOGUE;
    dlg.npc_index = 0;
    dlg.quest_id = dlg_quest;
    dlg.kind = kind;
    session->do_send(dlg.size, &dlg);
}

static void HandleQuestAction(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
    C2S_QuestAction* p = reinterpret_cast<C2S_QuestAction*>(ptr);
    const QuestDef* def = GetQuestDef(p->quest_id);
    if (!def || def->giver_npc != 0) return;

    // 보상이 걸린 동작이므로 근접성 재검증
    int gdx = abs(static_cast<int>(session->x) - static_cast<int>(QUEST_GIVER_X));
    int gdy = abs(static_cast<int>(session->y) - static_cast<int>(QUEST_GIVER_Y));
    if (max(gdx, gdy) > QUEST_INTERACT_RANGE) {
        SendSystemMessage(session, "You are too far from the Elder.");
        return;
    }

    if (p->action == 0) {
        // 수락
        bool ok = false;
        {
            lock_guard<mutex> lk(session->quest_lock);
            bool have = false;
            for (auto& q : session->quests) if (q.quest_id == def->id) { have = true; break; }
            bool prereq_ok = true;
            if (def->prereq_id >= 0) {
                prereq_ok = false;
                for (auto& q : session->quests)
                    if (q.quest_id == def->prereq_id && q.state == 1) { prereq_ok = true; break; }
            }
            if (!have && prereq_ok) {
                session->quests.push_back({ def->id, 0, (unsigned char)0 });
                ok = true;
            }
        }
        if (ok) {
            SendQuestUpdate(session, def->id, 0, def->target_count, 0);
            SendSystemMessage(session, "Quest accepted.");
        }
    }
    else if (p->action == 1) {
        // 보상 수령 (active + 카운트 충족 시에만)
        bool completed = false;
        {
            lock_guard<mutex> lk(session->quest_lock);
            for (auto& q : session->quests) {
                if (q.quest_id == def->id && q.state == 0 && q.kill_count >= def->target_count) {
                    q.state = 1;  // completed
                    completed = true;
                    break;
                }
            }
        }
        if (completed) {
            if (def->reward_exp > 0) LevelUpPlayer(session, def->reward_exp);
            if (def->reward_item_id >= 0 && def->reward_item_qty > 0) {
                bool added;
                { lock_guard<mutex> lk(session->inv_lock); added = AddToInventoryLocked(*session, def->reward_item_id, def->reward_item_qty); }
                if (added) {
                    SendInventory(session);
                } else {
                    // 인벤 가득 → 발밑에 드롭 (아이템이 사라지지 않도록)
                    int rdrop_id = g_next_drop_id.fetch_add(1);
                    short rx = session->x, ry = session->y;
                    {
                        lock_guard<mutex> lk(g_ground_mutex);
                        g_ground_items[rdrop_id] = GroundItem{ def->reward_item_id, def->reward_item_qty, rx, ry };
                    }
                    S2C_ItemDrop dpkt;
                    dpkt.size = sizeof(dpkt); dpkt.type = S2C_ITEM_DROP;
                    dpkt.drop_id = rdrop_id; dpkt.item_id = def->reward_item_id;
                    dpkt.x = rx; dpkt.y = ry;
                    BroadcastToSectorPlayers(rx, ry, dpkt);
                    g_timer_manager.Schedule(rdrop_id, TimerEventKind::GroundItemExpire, GROUND_ITEM_EXPIRE_MS);
                    SendSystemMessage(session, "Inventory full — reward dropped at your feet.");
                }
            }
            SendQuestUpdate(session, def->id, def->target_count, def->target_count, 1);
            SendSystemMessage(session, "Quest complete! Rewards granted.");
        }
    }
    else if (p->action == 2) {
        // 퀘스트 포기: 진행 중(state==0)인 퀘스트만 취소 가능
        bool abandoned = false;
        {
            lock_guard<mutex> lk(session->quest_lock);
            auto& qlist = session->quests;
            for (auto it = qlist.begin(); it != qlist.end(); ++it) {
                if (it->quest_id == def->id && it->state == 0) {
                    qlist.erase(it);
                    abandoned = true;
                    break;
                }
            }
        }
        if (abandoned) {
            // 클라이언트에 퀘스트 제거 알림 — kill_count=-1로 "removed" 시그널 사용
            SendQuestUpdate(session, def->id, -1, def->target_count, 0);
            SendSystemMessage(session, "Quest abandoned.");
        }
    }
}

static void HandleLogout(const shared_ptr<Player>& session, int client_id, unsigned char* ptr) {
#if VERBOSE_CLIENT_EVENTS
    cout << "[Logout] Client " << client_id << " requested logout." << endl;
#endif
    // closesocket → IO 실패 → disconnect 경로가 view_list 정리/Remove 전송 담당
    closesocket(session->socket);
}

void process_packet(int client_id, unsigned char* ptr) {
    PACKET_TYPE type = static_cast<PACKET_TYPE>(ptr[1]);

    shared_ptr<Player> session;
    {
        ClientMap::const_accessor a;
        if (!g_clients.find(a, client_id)) return;
        session = a->second;
    }

    // 완전 입장(스폰 완료) 전에는 C2S_LOGIN 외 모든 패킷 무시.
    // name이 아니라 spawned로 게이팅 — (로그인~OnPlayerSpawn) 사이 좌표 미설정 윈도우에서
    // 이동/공격이 처리돼 섹터/시야가 깨지는 것을 막는다.
    if (type != C2S_LOGIN) {
        if (!session->spawned.load()) return;
    }

    switch (type) {
    case C2S_LOGIN:
        HandleLogin(session, client_id, ptr);
        break;
    case C2S_MOVE:
    case C2S_TELEPORT:
        HandleMove(session, client_id, ptr, type == C2S_TELEPORT);
        break;
    case C2S_ATTACK:
        HandleAttack(session, client_id, ptr);
        break;
    case C2S_USE_SKILL:
        HandleUseSkill(session, client_id, ptr);
        break;
    case C2S_CHAT:
        HandleChat(session, client_id, ptr);
        break;
    case C2S_PARTY_INVITE:
        HandlePartyInvite(session, client_id, ptr);
        break;
    case C2S_PARTY_ACCEPT:
        HandlePartyAccept(session, client_id, ptr);
        break;
    case C2S_PARTY_REJECT:
        HandlePartyReject(session, client_id, ptr);
        break;
    case C2S_PARTY_LEAVE:
        HandlePartyLeave(session, client_id, ptr);
        break;
    // === Stage 8: 아이템 ===
    case C2S_PICKUP:
        HandlePickup(session, client_id, ptr);
        break;
    case C2S_USE_ITEM:
        HandleUseItem(session, client_id, ptr);
        break;
    case C2S_EQUIP_ITEM:
        HandleEquipItem(session, client_id, ptr);
        break;
    case C2S_UNEQUIP_ITEM:
        HandleUnequipItem(session, client_id, ptr);
        break;
    case C2S_DROP_ITEM:
        HandleDropItem(session, client_id, ptr);
        break;
    case C2S_QUEST_INTERACT:
        HandleQuestInteract(session, client_id, ptr);
        break;
    case C2S_QUEST_ACTION:
        HandleQuestAction(session, client_id, ptr);
        break;
    case C2S_LOGOUT:
        HandleLogout(session, client_id, ptr);
        break;
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
    // [perf] 샤딩된 타이머는 전역 발화 순서를 보장하지 않는다(샤드별/엔티티별 순서만 보존).
    // 따라서 이 콜백 기록 순서 검사는 정보성 지표로만 사용하고 PASS 기준에서는 제외한다.
    bool ordered = true;
    for (size_t i = 1; i < fires.size(); ++i) {
        if (fires[i].fire_time < fires[i - 1].fire_time) {
            ordered = false;
            cout << "[Timer Test] (info) cross-shard record out of order at index " << i << endl;
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
    cout << "[Timer Test] Record order (info, not graded after sharding): " << (ordered ? "in-order" : "cross-shard interleave") << endl;
    cout << "[Timer Test] Lag avg=" << avg_lag << "ms, max=" << max_lag
         << "ms, over " << TOLERANCE_MS << "ms tolerance: " << over_tolerance << endl;

    bool pass = (fired == N) && (over_tolerance == 0);
    cout << "[Timer Test] Overall: " << (pass ? "PASS" : "FAIL") << endl;
    return pass ? 0 : 1;
}
