#pragma once

// Stage 6: 월드 장애물 비트맵.
// WORLD_WIDTH x WORLD_HEIGHT 비트 = 2000*2000/8 = 500,000 byte.
// 1 = blocked, 0 = walkable. 월드 밖 좌표는 항상 blocked로 취급.

#include "../protocol_2026.h"
#include "GameConfig.h"
#include <cstddef>
#include <cstdint>

namespace Map {

constexpr int W = WORLD_WIDTH;
constexpr int H = WORLD_HEIGHT;
constexpr std::size_t BITMAP_BYTES = (static_cast<std::size_t>(W) * H + 7) / 8;

extern std::uint8_t g_obstacle_bits[BITMAP_BYTES];

// data/obstacles.txt 같은 rect 스크립트 로드.
// 반환: 적용된 rect 수, -1 = 파일 열기 실패.
int LoadObstacles(const char* path);

inline bool InBounds(int x, int y) {
    return x >= 0 && y >= 0 && x < W && y < H;
}

// Stage 6.4: 절차적 장식 칸 여부 (클라/서버 동일 해시).
// 마을 영역은 제외. 좌표 해시 하위 8비트 < 12 (~4.7%) 일 때 장식.
// 장식은 시각만 아니라 실제 장애물로 취급됨 (캐릭/NPC 통과 불가).
inline bool IsProceduralDeco(int x, int y) {
    if (x >= VILLAGE_X1 && x < VILLAGE_X2 && y >= VILLAGE_Y1 && y < VILLAGE_Y2) return false;
    unsigned int dh = (unsigned int)(x * 0x9E3779B1u) ^ (unsigned int)(y * 0x85EBCA77u);
    return (dh & 0xFFu) < 12u;
}

inline bool IsBlocked(int x, int y) {
    if (!InBounds(x, y)) return true;
    std::size_t idx = static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x);
    if (((g_obstacle_bits[idx >> 3] >> (idx & 7)) & 1u) != 0) return true;
    if (IsProceduralDeco(x, y)) return true;
    return false;
}

inline bool IsWalkable(int x, int y) {
    return !IsBlocked(x, y);
}

} // namespace Map
