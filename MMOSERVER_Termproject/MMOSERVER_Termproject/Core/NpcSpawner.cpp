#define _CRT_SECURE_NO_WARNINGS
#include "NpcSpawner.h"
#include "World.h"
#include "Map.h"
#include "GameConfig.h"
#include "../protocol_2026.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <iostream>

namespace {

// 텍스트 토큰 → enum 파서
bool ParseType(const char* s, NpcType& out) {
    if (_stricmp(s, "Peace") == 0) { out = NpcType::Peace; return true; }
    if (_stricmp(s, "Agro")  == 0) { out = NpcType::Agro;  return true; }
    if (_stricmp(s, "Boss")  == 0) { out = NpcType::Boss;  return true; }
    return false;
}
bool ParseMoveMode(const char* s, NpcMoveMode& out) {
    if (_stricmp(s, "Fixed")   == 0) { out = NpcMoveMode::Fixed;   return true; }
    if (_stricmp(s, "Roaming") == 0) { out = NpcMoveMode::Roaming; return true; }
    return false;
}

} // anon

int LoadNpcSpawnScript(const char* path) {
    FILE* fp = std::fopen(path, "r");
    if (!fp) {
        std::cerr << "[NpcSpawner] Failed to open " << path << std::endl;
        return -1;
    }

    // 결정적 스폰을 위해 고정 시드 사용 (동일 스크립트는 매번 동일 배치)
    std::mt19937 rng(0xA37E1A20u);

    int spawn_index = 0;
    char line[512];
    int line_no = 0;

    while (std::fgets(line, sizeof(line), fp)) {
        ++line_no;
        // 주석/빈 줄 스킵
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        int count = 0;
        char type_s[32], move_s[32], name_s[32];
        int visual_id = 0;
        int script_level = 1, script_hp = 100;
        int ax1 = 0, ay1 = 0, ax2 = 0, ay2 = 0;
        int matched = std::sscanf(p, "%d %31s %31s %d %31s %d %d %d %d %d %d",
            &count, type_s, move_s, &visual_id, name_s,
            &script_level, &script_hp, &ax1, &ay1, &ax2, &ay2);
        if (matched != 11) {
            std::cerr << "[NpcSpawner] Line " << line_no << " parse error: " << line;
            continue;
        }

        NpcType type;
        NpcMoveMode mode;
        if (!ParseType(type_s, type) || !ParseMoveMode(move_s, mode)) {
            std::cerr << "[NpcSpawner] Line " << line_no << " unknown enum: " << type_s << " " << move_s << std::endl;
            continue;
        }
        if (ax1 < 0) ax1 = 0;
        if (ay1 < 0) ay1 = 0;
        if (ax2 > WORLD_WIDTH)  ax2 = WORLD_WIDTH;
        if (ay2 > WORLD_HEIGHT) ay2 = WORLD_HEIGHT;
        if (ax2 <= ax1 || ay2 <= ay1) {
            std::cerr << "[NpcSpawner] Line " << line_no << " invalid area." << std::endl;
            continue;
        }

        std::uniform_int_distribution<int> dist_x(ax1, ax2 - 1);
        std::uniform_int_distribution<int> dist_y(ay1, ay2 - 1);

        for (int i = 0; i < count; ++i) {
            if (spawn_index >= NUM_NPCS) {
                std::cerr << "[NpcSpawner] NUM_NPCS capacity reached at line " << line_no << std::endl;
                std::fclose(fp);
                g_npc_count = spawn_index;
                return spawn_index;
            }

            // Fixed 모드: area 중심에 스폰 (위치가 예측 가능해야 보스 텔레포트가 정확함)
            // Roaming 모드: 무작위 추첨 후 IsBlocked면 재시도
            short sx = 0, sy = 0;
            bool placed = false;
            if (mode == NpcMoveMode::Fixed) {
                short cx = static_cast<short>((ax1 + ax2) / 2);
                short cy = static_cast<short>((ay1 + ay2) / 2);
                if (!Map::IsBlocked(cx, cy)) {
                    sx = cx; sy = cy; placed = true;
                } else {
                    // 중심이 막혀 있으면 주변 탐색
                    for (int r = 1; r <= 5 && !placed; ++r) {
                        for (int dy2 = -r; dy2 <= r && !placed; ++dy2) {
                            for (int dx2 = -r; dx2 <= r && !placed; ++dx2) {
                                short tx = cx + static_cast<short>(dx2);
                                short ty = cy + static_cast<short>(dy2);
                                if (!Map::IsBlocked(tx, ty)) { sx=tx; sy=ty; placed=true; }
                            }
                        }
                    }
                }
            } else {
                for (int attempt = 0; attempt < 32; ++attempt) {
                    short tx = static_cast<short>(dist_x(rng));
                    short ty = static_cast<short>(dist_y(rng));
                    if (!Map::IsBlocked(tx, ty)) {
                        sx = tx; sy = ty;
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed) {
                // fallback: area 선형 스캔. area 전체가 막혀 있으면 spawn 포기.
                for (int yy = ay1; yy < ay2 && !placed; ++yy) {
                    for (int xx = ax1; xx < ax2; ++xx) {
                        if (!Map::IsBlocked(xx, yy)) {
                            sx = static_cast<short>(xx);
                            sy = static_cast<short>(yy);
                            placed = true;
                            break;
                        }
                    }
                }
            }
            if (!placed) {
                std::cerr << "[NpcSpawner] No walkable tile in area at line "
                          << line_no << " — skipping NPC slot." << std::endl;
                continue;  // spawn_index 유지 — 이 슬롯은 건너뜀
            }

            NPC& n = g_npcs[spawn_index];
            n.id = NPC_ID_START + spawn_index;
            n.type = type;
            n.move_mode = mode;
            n.state = (mode == NpcMoveMode::Roaming) ? NpcFsmState::Roaming : NpcFsmState::Idle;
            n.visual_id = visual_id;
            n.area_x1 = static_cast<short>(ax1);
            n.area_y1 = static_cast<short>(ay1);
            n.area_x2 = static_cast<short>(ax2);
            n.area_y2 = static_cast<short>(ay2);
            n.spawn_x = sx;
            n.spawn_y = sy;
            n.x = sx;
            n.y = sy;
            n.target_id = -1;
            n.active.store(false);
            n.boss_tick_count.store(0);

            // 보스는 GameConfig 전용 스탯, 일반 NPC는 스크립트 값 사용
            if (type == NpcType::Boss) {
                n.hp = BOSS_MAX_HP;
                n.max_hp = BOSS_MAX_HP;
                n.level = static_cast<unsigned char>(BOSS_LEVEL);
            } else {
                n.hp = script_hp;
                n.max_hp = script_hp;
                n.level = static_cast<unsigned char>(script_level);
            }

            // name: "<prefix>_<id마지막 5자리>" 형태로 구별 가능하게
            std::snprintf(n.name, sizeof(n.name), "%s_%05d", name_s, spawn_index % 100000);

            ++spawn_index;
        }
    }
    std::fclose(fp);

    g_npc_count = spawn_index;
    std::cout << "[NpcSpawner] Spawned " << spawn_index << " NPCs from " << path << std::endl;
    return spawn_index;
}
