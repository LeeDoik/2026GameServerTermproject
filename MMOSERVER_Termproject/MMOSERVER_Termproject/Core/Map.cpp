#define _CRT_SECURE_NO_WARNINGS
#include "Map.h"

#include <cstdio>
#include <cstring>
#include <iostream>

namespace Map {

std::uint8_t g_obstacle_bits[BITMAP_BYTES] = { 0 };

namespace {

inline void MarkBlocked(int x, int y) {
    if (!InBounds(x, y)) return;
    std::size_t idx = static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x);
    g_obstacle_bits[idx >> 3] |= static_cast<std::uint8_t>(1u << (idx & 7));
}

} // anon

int LoadObstacles(const char* path) {
    std::memset(g_obstacle_bits, 0, sizeof(g_obstacle_bits));

    FILE* fp = std::fopen(path, "r");
    if (!fp) return -1;

    int rect_count = 0;
    long long blocked_tiles = 0;
    char line[256];
    int line_no = 0;

    while (std::fgets(line, sizeof(line), fp)) {
        ++line_no;
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char kind[16];
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (std::sscanf(p, "%15s %d %d %d %d", kind, &x1, &y1, &x2, &y2) != 5) {
            std::cerr << "[Map] Line " << line_no << " parse error: " << line;
            continue;
        }
        if (_stricmp(kind, "rect") != 0) {
            std::cerr << "[Map] Line " << line_no << " unknown kind: " << kind << std::endl;
            continue;
        }

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > W) x2 = W;
        if (y2 > H) y2 = H;
        if (x2 <= x1 || y2 <= y1) {
            std::cerr << "[Map] Line " << line_no << " empty rect ignored." << std::endl;
            continue;
        }

        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) {
                MarkBlocked(x, y);
            }
        }
        ++rect_count;
        blocked_tiles += static_cast<long long>(x2 - x1) * (y2 - y1);
    }
    std::fclose(fp);

    double pct = 100.0 * static_cast<double>(blocked_tiles)
        / static_cast<double>(static_cast<long long>(W) * H);
    std::cout << "[Map] Loaded " << rect_count << " rects, "
              << blocked_tiles << " blocked tiles ("
              << pct << "% of world)" << std::endl;
    return rect_count;
}

} // namespace Map
