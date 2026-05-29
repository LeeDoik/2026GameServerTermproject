#include "LuaVM.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

namespace {

// 테이블에 정수 필드 세팅 헬퍼
inline void TableSetInt(lua_State* L, const char* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

} // anon

LuaVM::LuaVM() {
    L_ = luaL_newstate();
}

LuaVM::~LuaVM() {
    std::lock_guard<std::mutex> lock(mu_);
    if (L_) {
        lua_close(static_cast<lua_State*>(L_));
        L_ = nullptr;
    }
}

bool LuaVM::OpenStdLibs() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!L_) return false;
    luaL_openlibs(static_cast<lua_State*>(L_));
    return true;
}

bool LuaVM::DoFile(const char* path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!L_) {
        last_error_ = "lua_State not initialized";
        return false;
    }
    lua_State* L = static_cast<lua_State*>(L_);
    if (luaL_dofile(L, path) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        last_error_ = msg ? msg : "(unknown lua error)";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool LuaVM::CallGlobalVoid(const char* func_name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!L_) {
        last_error_ = "lua_State not initialized";
        return false;
    }
    lua_State* L = static_cast<lua_State*>(L_);
    lua_getglobal(L, func_name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        last_error_ = std::string("global is not a function: ") + func_name;
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        last_error_ = msg ? msg : "(unknown lua pcall error)";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void LuaVM::SetGlobalInt(const char* name, long long value) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!L_) return;
    lua_State* L = static_cast<lua_State*>(L_);
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setglobal(L, name);
}

bool LuaVM::NpcTick(const NpcTickContext& ctx, NpcTickResult& out) {
    // 안전 디폴트: 정지 + target 유지
    out.dx = 0;
    out.dy = 0;
    out.target_id = ctx.target_id;

    // [perf] 락 없음: 이 인스턴스는 워커 스레드 전용(thread_local)이라 동시 호출이 없다.
    if (!L_) {
        last_error_ = "lua_State not initialized";
        return false;
    }
    lua_State* L = static_cast<lua_State*>(L_);

    lua_getglobal(L, "OnTick");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        last_error_ = "global OnTick is not a function";
        return false;
    }

    // ctx 테이블 push
    lua_createtable(L, 0, 17);
    TableSetInt(L, "id", ctx.id);
    TableSetInt(L, "npc_type", ctx.npc_type);
    TableSetInt(L, "move_mode", ctx.move_mode);
    TableSetInt(L, "x", ctx.x);
    TableSetInt(L, "y", ctx.y);
    TableSetInt(L, "spawn_x", ctx.spawn_x);
    TableSetInt(L, "spawn_y", ctx.spawn_y);
    TableSetInt(L, "target_id", ctx.target_id);
    TableSetInt(L, "target_x", ctx.target_x);
    TableSetInt(L, "target_y", ctx.target_y);
    TableSetInt(L, "nearest_id", ctx.nearest_id);
    TableSetInt(L, "nearest_x", ctx.nearest_x);
    TableSetInt(L, "nearest_y", ctx.nearest_y);
    TableSetInt(L, "nearest_dist", ctx.nearest_dist);
    TableSetInt(L, "hp", ctx.hp);
    TableSetInt(L, "max_hp", ctx.max_hp);
    TableSetInt(L, "boss_tick_count", ctx.boss_tick_count);

    // OnTick(ctx) → 3 returns: dx, dy, target_id
    if (lua_pcall(L, 1, 3, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        last_error_ = msg ? msg : "(unknown OnTick error)";
        lua_pop(L, 1);
        return false;
    }

    // 반환값: -3=dx, -2=dy, -1=target_id (모두 integer 기대)
    int dx = static_cast<int>(luaL_optinteger(L, -3, 0));
    int dy = static_cast<int>(luaL_optinteger(L, -2, 0));
    int tid = static_cast<int>(luaL_optinteger(L, -1, ctx.target_id));
    lua_pop(L, 3);

    // -1, 0, +1로 clamp (스크립트 버그 방어)
    if (dx < -1) dx = -1; else if (dx > 1) dx = 1;
    if (dy < -1) dy = -1; else if (dy > 1) dy = 1;
    out.dx = dx;
    out.dy = dy;
    out.target_id = tid;
    out.chat_id = 0;
    out.do_boss_aoe = 0;
    return true;
}

bool LuaVM::BossTick(const NpcTickContext& ctx, NpcTickResult& out) {
    // 안전 디폴트
    out.dx = 0; out.dy = 0; out.target_id = ctx.target_id;
    out.chat_id = 0; out.do_boss_aoe = 0;

    // [perf] 락 없음: 워커 스레드 전용(thread_local) 인스턴스.
    if (!L_) { last_error_ = "lua_State not initialized"; return false; }
    lua_State* L = static_cast<lua_State*>(L_);

    lua_getglobal(L, "OnBossTick");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        last_error_ = "global OnBossTick is not a function";
        return false;
    }

    lua_createtable(L, 0, 17);
    TableSetInt(L, "id", ctx.id);
    TableSetInt(L, "npc_type", ctx.npc_type);
    TableSetInt(L, "move_mode", ctx.move_mode);
    TableSetInt(L, "x", ctx.x);
    TableSetInt(L, "y", ctx.y);
    TableSetInt(L, "spawn_x", ctx.spawn_x);
    TableSetInt(L, "spawn_y", ctx.spawn_y);
    TableSetInt(L, "target_id", ctx.target_id);
    TableSetInt(L, "target_x", ctx.target_x);
    TableSetInt(L, "target_y", ctx.target_y);
    TableSetInt(L, "nearest_id", ctx.nearest_id);
    TableSetInt(L, "nearest_x", ctx.nearest_x);
    TableSetInt(L, "nearest_y", ctx.nearest_y);
    TableSetInt(L, "nearest_dist", ctx.nearest_dist);
    TableSetInt(L, "hp", ctx.hp);
    TableSetInt(L, "max_hp", ctx.max_hp);
    TableSetInt(L, "boss_tick_count", ctx.boss_tick_count);

    // OnBossTick(ctx) → 5 returns: dx, dy, target_id, chat_id, do_boss_aoe
    if (lua_pcall(L, 1, 5, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        last_error_ = msg ? msg : "(unknown OnBossTick error)";
        lua_pop(L, 1);
        return false;
    }

    int dx   = static_cast<int>(luaL_optinteger(L, -5, 0));
    int dy   = static_cast<int>(luaL_optinteger(L, -4, 0));
    int tid  = static_cast<int>(luaL_optinteger(L, -3, ctx.target_id));
    int cid  = static_cast<int>(luaL_optinteger(L, -2, 0));
    int aoe  = static_cast<int>(luaL_optinteger(L, -1, 0));
    lua_pop(L, 5);

    if (dx < -1) dx = -1; else if (dx > 1) dx = 1;
    if (dy < -1) dy = -1; else if (dy > 1) dy = 1;
    out.dx = dx; out.dy = dy; out.target_id = tid;
    out.chat_id = cid; out.do_boss_aoe = (aoe != 0) ? 1 : 0;
    return true;
}
