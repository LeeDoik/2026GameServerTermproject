#include "AStar.h"
#include "Map.h"

#include <algorithm>
#include <cstdlib>
#include <queue>
#include <unordered_map>
#include <vector>

namespace AStar {

namespace {

// 검색 영역 마진 (start/goal 포함 bbox에 이만큼 확장)
constexpr int BBOX_MARGIN = 8;
// 처리 노드 수 상한 (200K NPC가 동시에 막혀도 catastrophic하지 않도록)
constexpr int MAX_NODES = 256;

struct Node {
    int x, y;
    int g;      // 시작에서 현재까지 비용 (manhattan)
    int f;      // g + heuristic
};

struct NodeGreater {
    bool operator()(const Node& a, const Node& b) const { return a.f > b.f; }
};

inline int Key(int x, int y) {
    // (x, y) → x * Map::H + y. WORLD = 2000x2000이므로 int 범위 안전.
    return x * Map::H + y;
}

inline int Manhattan(int x1, int y1, int x2, int y2) {
    return std::abs(x1 - x2) + std::abs(y1 - y2);
}

} // anon

bool AStarStep(int sx, int sy, int gx, int gy, int& out_dx, int& out_dy) {
    out_dx = 0;
    out_dy = 0;
    if (sx == gx && sy == gy) return true;

    // 검색 영역 bbox
    int min_x = std::min(sx, gx) - BBOX_MARGIN;
    int max_x = std::max(sx, gx) + BBOX_MARGIN;
    int min_y = std::min(sy, gy) - BBOX_MARGIN;
    int max_y = std::max(sy, gy) + BBOX_MARGIN;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= Map::W) max_x = Map::W - 1;
    if (max_y >= Map::H) max_y = Map::H - 1;

    std::priority_queue<Node, std::vector<Node>, NodeGreater> open;
    std::unordered_map<int, int> g_score;   // key → best g
    std::unordered_map<int, int> came_from; // key → parent key

    int sk = Key(sx, sy);
    int gk = Key(gx, gy);
    g_score[sk] = 0;
    open.push(Node{ sx, sy, 0, Manhattan(sx, sy, gx, gy) });

    constexpr int dx4[4] = { 1, -1, 0, 0 };
    constexpr int dy4[4] = { 0, 0, 1, -1 };

    int processed = 0;
    bool found = false;

    while (!open.empty() && processed < MAX_NODES) {
        Node cur = open.top();
        open.pop();

        int ck = Key(cur.x, cur.y);
        // stale 엔트리: 더 좋은 g로 이미 처리됨
        auto git = g_score.find(ck);
        if (git == g_score.end() || cur.g > git->second) continue;
        ++processed;

        if (ck == gk) { found = true; break; }

        for (int i = 0; i < 4; ++i) {
            int nx = cur.x + dx4[i];
            int ny = cur.y + dy4[i];
            if (nx < min_x || nx > max_x || ny < min_y || ny > max_y) continue;
            // 목표 칸은 walkable 여부 무관 (플레이어가 거기 서 있을 수 있음)
            if (!(nx == gx && ny == gy) && Map::IsBlocked(nx, ny)) continue;

            int ng = cur.g + 1;
            int nk = Key(nx, ny);
            auto nit = g_score.find(nk);
            if (nit != g_score.end() && ng >= nit->second) continue;
            g_score[nk] = ng;
            came_from[nk] = ck;
            int nf = ng + Manhattan(nx, ny, gx, gy);
            open.push(Node{ nx, ny, ng, nf });
        }
    }

    if (!found) return false;

    // 경로 추적: gk → ... → sk. sk 직후 첫 칸을 first_step으로.
    int trace = gk;
    int first_step = gk;
    while (trace != sk) {
        first_step = trace;
        auto it = came_from.find(trace);
        if (it == came_from.end()) return false;  // 끊김 (이론상 발생 X)
        trace = it->second;
    }
    int fx = first_step / Map::H;
    int fy = first_step % Map::H;
    out_dx = fx - sx;
    out_dy = fy - sy;
    return true;
}

} // namespace AStar
