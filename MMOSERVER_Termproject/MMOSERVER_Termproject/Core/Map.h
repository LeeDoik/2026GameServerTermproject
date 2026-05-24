#pragma once

// Stage 6: 월드 장애물 비트맵.
// WORLD_WIDTH x WORLD_HEIGHT 비트 = 2000*2000/8 = 500,000 byte.
// 1 = blocked, 0 = walkable. 월드 밖 좌표는 항상 blocked로 취급.

#include "../protocol_2026.h"
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

inline bool IsBlocked(int x, int y) {
    if (!InBounds(x, y)) return true;
    std::size_t idx = static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x);
    return ((g_obstacle_bits[idx >> 3] >> (idx & 7)) & 1u) != 0;
}

inline bool IsWalkable(int x, int y) {
    return !IsBlocked(x, y);
}

} // namespace Map
