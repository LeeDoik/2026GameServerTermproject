#pragma once

// PDF 기본 스펙 기반 게임 규칙 상수.
// protocol_2026.h는 제공된 프로토콜 정의용이므로 건드리지 않고, 게임 규칙은 여기로 분리.

// 이동: 0.5초에 한 칸 (몬스터, 플레이어 모두)
constexpr int NPC_TICK_INTERVAL_MS = 500;
constexpr int PLAYER_MOVE_INTERVAL_MS = 500;  // 클라이언트 보간용 참고값

// Agro 트리거: 11x11 영역 = 중심으로부터 ±5 (chebyshev distance)
constexpr int AGRO_DETECT_RANGE = 5;

// 로밍 범위: 스폰 위치 중심 20x20 = ±10
constexpr int ROAM_AREA_RANGE = 10;

// 사망 후 30초 부활 (Stage 5에서 사용)
constexpr int NPC_RESPAWN_MS = 30000;

// 5초마다 HP 10% 회복 (Stage 5)
constexpr int HP_REGEN_INTERVAL_MS = 5000;
constexpr int HP_REGEN_PERCENT = 10;

// 경험치 공식: 레벨^2 * 2 (Agro x2, Roaming x2 — Stage 5 적용)
constexpr int BASE_EXP_MULTIPLIER = 2;

// 전투
constexpr int ATTACK_INTERVAL_MS = 1000;     // 플레이어 공격 쿨타임 (PDF: 1초/공격)
constexpr int BASE_DAMAGE_PER_LEVEL = 10;    // 플레이어 데미지 = level * 10
constexpr int NPC_ATTACK_INTERVAL_MS = 1000; // NPC 공격 쿨타임
constexpr int NPC_BASE_DAMAGE = 5;           // NPC 데미지 = level * 5 (플레이어보다 약하게)

// 사망 시 리스폰 위치 (Aetheria Village 분수 남쪽 — 분수 sprite 위에 안 겹치게)
constexpr short PLAYER_SPAWN_X = 1000;
constexpr short PLAYER_SPAWN_Y = 1010;

// 시작 마을(Aetheria Village) 영역. 외벽은 obstacles.txt에 정의.
// 신규 캐릭은 이 영역 안에서 walkable 좌표로 스폰.
constexpr short VILLAGE_X1 = 960;
constexpr short VILLAGE_Y1 = 960;
constexpr short VILLAGE_X2 = 1040;
constexpr short VILLAGE_Y2 = 1040;

// Stage 6.3: DB 자동 저장 주기 (30초)
constexpr int PLAYER_AUTO_SAVE_INTERVAL_MS = 30000;
