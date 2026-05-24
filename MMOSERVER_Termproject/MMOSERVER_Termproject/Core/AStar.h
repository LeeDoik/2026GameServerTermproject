#pragma once

// Stage 6.2: 격자 A* — NPC Agro 추적 시 장애물 우회.
// 호출 빈도 제어를 위해 Lua가 반환한 step이 막힌 경우에만 호출.
// 4방향(상하좌우, dx+dy의 절댓값 합 1)으로 경로 첫 한 칸을 반환.
// 검색 영역은 (start, goal)을 포함하는 bbox + margin으로 한정, max nodes 제한.

namespace AStar {

// 최단 경로의 첫 한 칸 방향을 out_dx, out_dy에 채워 반환.
// 반환값: 경로를 찾았으면 true. 못 찾았으면 false (out_dx=out_dy=0).
// 시작=목표이면 true, (0,0) 반환.
// 목표 칸이 막혀있어도 그 직전 칸까지의 경로는 탐색 (마지막 step만 사용).
bool AStarStep(int sx, int sy, int gx, int gy, int& out_dx, int& out_dy);

} // namespace AStar
