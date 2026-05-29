#pragma once

#include <string>
#include <mutex>

// Lua 5.5 인터프리터 래퍼.
//
// 스레드 모델: lua_State는 thread-safe가 아니다. [perf] AI 핫패스(NpcTick/BossTick)는
// 워커 스레드당 1개씩 두는 thread_local 인스턴스에서만 호출하므로 동시 접근이 없고,
// 이 두 함수는 락을 잡지 않는다(단일 전역 락 직렬화 병목 제거).
// 부팅 시 단일 스레드 초기화 경로(OpenStdLibs/DoFile/SetGlobalInt)는 mu_로 보호 유지.

// NPC AI tick 입력: C++가 Lua로 넘기는 정보
struct NpcTickContext {
    int id;
    int npc_type;        // 0=Peace, 1=Agro, 2=Boss
    int move_mode;       // 0=Fixed, 1=Roaming
    int x, y;
    int spawn_x, spawn_y;
    int target_id;       // 현재 추적 중 (-1 = 없음, 첫 인식 후 유지)
    int target_x, target_y;  // 추적 대상의 현재 위치 (-1 = 시야 밖 / 없음)
    int nearest_id;      // 가장 가까운 viewer (Agro 트리거 판정용, -1 = view 비어있음)
    int nearest_x, nearest_y;
    int nearest_dist;    // chebyshev distance
    // 보스 전용 필드 (Boss 외 타입에서는 기본값 전달)
    int hp;
    int max_hp;
    int boss_tick_count;
};

// NPC AI tick 결과: Lua가 C++로 돌려주는 결정
struct NpcTickResult {
    int dx, dy;          // 한 칸 이동량 (-1, 0, +1)
    int target_id;       // 새 target_id (-1이면 추적 해제)
    // 보스 전용 결과 (일반 NPC는 0)
    int chat_id;         // 0=없음, 1~4 = 보스 대사 인덱스
    int do_boss_aoe;     // 0=없음, 1=이번 틱 AoE 발동
};

class LuaVM {
public:
    LuaVM();
    ~LuaVM();
    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    bool OpenStdLibs();
    bool DoFile(const char* path);
    bool CallGlobalVoid(const char* func_name);

    // Lua 글로벌 정수 상수 설정 (예: TYPE_AGRO, AGRO_RANGE 등 스펙 노출)
    void SetGlobalInt(const char* name, long long value);

    // NPC AI tick: Lua의 OnTick(ctx) 호출. 실패 시 결과는 정지(dx=dy=0, target_id 유지)
    bool NpcTick(const NpcTickContext& ctx, NpcTickResult& out);

    // 보스 AI tick: Lua의 OnBossTick(ctx) 호출. 5개 반환값 (dx,dy,target_id,chat_id,do_boss_aoe)
    bool BossTick(const NpcTickContext& ctx, NpcTickResult& out);

    const std::string& GetLastError() const { return last_error_; }

private:
    void* L_;
    std::mutex mu_;       // 모든 lua 호출 직렬화
    std::string last_error_;
};
