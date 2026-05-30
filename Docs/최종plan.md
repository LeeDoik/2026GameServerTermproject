# Aetheria Online MMO 서버 재설계 계획

> 최종 업데이트: 2026-05-30
> Stage 1~6 완료. Stage 7.1 스킬 / 7.2 보스 / 7.3 클라 이펙트 / 7.4 파티 / 7.5 클라 메시지·EXP바 완료. Stage 8 아이템 시스템(소모·누적·인벤·장착) 완료. Stage 9 퀘스트 시스템(슬레이·대화·연쇄) 완료. SQL Server 백엔드는 선택사항.

---

## 0. 진행 현황 (2026-05-24)

| 단계 | 상태 | 핵심 결과 |
|---|---|---|
| Stage 1 — 안정성 핫픽스 | ✅ 완료 | 30초 스트레스 0 에러/0 크래시 |
| Stage 2 — Entity + view_list + ObjectPool | ✅ 완료 | 시야 경계 Remove 누락 버그 해결 |
| Stage 2 (추가) — concurrent_hash_map 교체 | ✅ 완료 | shared_mutex 글로벌 락 제거 |
| Stage 3 — TimerManager (priority_queue + cv) | ✅ 완료 | 1000/1000 이벤트, max lag 16ms |
| Stage 4 — NPC 200K 스폰 + AI + Lazy 시야 | ✅ 완료 | 200K NPC + 1,092 클라 churn / 0 error |
| Stage 4.5 — Lua 5.5 인터프리터 연동 | ✅ 완료 | npc_ai.lua 로드 + Hello() 호출 OK |
| Stage 4.6 — NPC AI Lua 이관 + PDF 스펙 정합 | ✅ 완료 | 0.5s tick / Agro 11x11 / 로밍 20x20 / 첫 인식 target 고정 |
| Stage 5 — 전투 + HP/EXP + 사망/리스폰(30s) + 채팅 | ✅ 완료 | 프로토콜 5종 추가, 클라 시각/HUD/이펙트 통합, 588 connect / 0 error |
| Stage 6.1 — 장애물 맵 (Map.h/.cpp + obstacles.txt) | ✅ 완료 | 64 rect / 5.18% 점유, 200K NPC 회피 스폰, 30초 stress 0 신규 에러 |
| Stage 6.2 — A* 길찾기 (4방향 격자, bbox+8 / 256 노드 한도) | ✅ 완료 | NPC Agro 추적 시 막힌 경로 자동 우회. 1767줄 / 0 신규 에러 |
| Stage 6.3 — DB 영속성 (JSON 파일 stub + IDbBackend 추상화) | ✅ 완료 | LOGIN 비동기화, 30초간 586개 JSON 저장, 1765줄 / 0 신규 에러 |
| Stage 7.1 — 스킬 시스템 (AoE/Line/Heal) | ✅ 완료 | Q/W/E 3스킬, 서버 핸들러 + 클라 이펙트/HUD |
| Stage 7.2 — 보스 패턴 (이동/채팅/AoE/2단계) | ✅ 완료 | 바이옴별 보스 4마리, Lua 2단계 분노 AI + 광역공격 |
| Stage 7.3 — 클라 이펙트 완성도 강화 | ✅ 완료 | 공격 이펙트 분리(zap/sandblast), 데미지 popup, 화면흔들기, HP보간/저체력경고, Q스킬 5×5 폭발 |
| Stage 7.4 — 파티 시스템 (EXP 분배 + 미니맵 + HP 바) | ✅ 완료 | /invite·/accept·/reject·/leave 명령, EXP 균등분배, 미니맵 청록 표시, 파티원 HP 바 HUD |
| Stage 8 — 아이템 시스템 (소모/누적/인벤/장착) | ✅ 완료 | 카탈로그 8종(items.txt), NPC 드롭+G줍기, I 인벤 패널, 무기 공격력/방어구 max_hp, DB 영속성. 30초 stress 0 신규 에러 |
| Stage 9 — 퀘스트 시스템 (슬레이/대화/연쇄) | ✅ 완료 | quests.txt 3단 연쇄, 처치 카운트 훅 3곳, 장로 대화 UI(T/J/Y), DB 영속성. 3종 빌드 0에러, 부팅 `[Quest] Loaded 3 quest defs.` / stderr 0 |

**현재 부하 테스트 결과** (Stage 6.1 완료 시점, 2026-05-24): Release x64 기준 30초간 약 580+ connect / 신규 stderr 에러 0건. 200K NPC 활성 + 64개 장애물 rect (5.18% 점유) + 충돌 체크 통합 상태에서 Stage 5 대비 성능 회귀 없음.

---

## 1. 현황 파악 요약 (분석 시점 기준 — 이후 변경 사항은 섹션 3 참조)

### 1.1 과제 명세 핵심 (PDF 기반)
- **기본 스펙**: 2000x2000 맵, 20x20 윈도우(시야 15x15), 장애물, 0.5초/칸 이동, 20만 NPC, A* 길찾기, Peace/Agro·고정/로밍, DB 저장, Script 로딩
- **평가**: 구현 50% + 성능 20%(동접 5,000 기준) + 설명서 10% + 게임성 40%(추가 요소 최대 40점)
- **핵심 제약**: IOCP 패킷 조립 / 타이머 / 스크립트 / DB 연동 코드를 실습 코드(`Docs/npc.cpp`)에서 그대로 가져오면 **20% 감점**. 단 시야 처리(Sector)는 참고 가능

### 1.2 기존 코드 베이스 (분석 시점 526줄, 현재 ~750줄)
- **네트워킹**: Windows IOCP + AcceptEx, 단일 listen 소켓
- **수신 재조립**: 자체 `RingBuffer` 클래스 (표절 회피 핵심, 그대로 유지)
- **공간 분할**: `Sector g_sectors[100][100]` (SECTOR_SIZE=20)

### 1.3 분석 시점의 주요 문제점 → 해결 상태

| # | 문제 | 해결 단계 |
|---|------|---|
| 1 | `do_send`가 매번 `new OVERLAPPED_EX` | Stage 2 Phase A (ObjectPool) |
| 2 | `SendToSector`가 섹터 락 보유 중 `do_send` 호출 | Stage 1 |
| 3 | 시야 경계 처리 시 원거리 entity Remove 누락 | Stage 2 Phase C (view_list 차분) |
| 4 | 세션에 view_list 없음, 9섹터 풀스캔 비효율 | Stage 2 Phase C |
| 5 | `S2C_ATTACK/DAMAGE/RESPAWN/...` 패킷 미정의 | Stage 5 예정 |
| 6 | NPC/AI/타이머/A*/DB/Script/전투 미구현 | Stage 3 (Timer) 완료, 나머지 4-6 예정 |
| 7 | `unsafe_erase` UB | Stage 1 (concurrent_hash_map으로 해결) |
| 8 | STRESS_TEST 빌드 불가 | Stage 1 |
| 9 | `g_clients.size()` 비효율 | concurrent_hash_map 교체로 자연 해결 |

### 1.4 보존된 자산
- `RingBuffer` 클래스 — 그대로 유지
- IOCP/AcceptEx 부트스트랩 — 거의 그대로
- Sector 구조와 좌표 매크로 — 그대로
- `protocol_2026.h`의 통합 객체 패킷 (`S2C_AddObject/MoveObject/RemoveObject`) — 그대로

---

## 2. 재설계 목표 아키텍처

### 2.1 레이어 구조
```
[Network Layer]   IOCP + AcceptEx + per-session RingBuffer (유지)
        ↓ (packet queue)
[Dispatcher]      packet_type → handler 라우팅
        ↓
[Game Layer]      World ↔ Sector ↔ Entity(Player/NPC) ↔ AI(FSM+A*)
        ↑                                  ↑
[Timer Layer]     std::priority_queue + condition_variable + dispatcher thread  ✅
        ↓ (PostQueuedCompletionStatus → IO_TIMER 이벤트)  ✅
[Persistence]     비동기 DB 워커 큐 (ODBC) + Script Loader (JSON)
```

### 2.2 핵심 설계 원칙
1. **Entity 통합 모델**: Player와 NPC 모두 `Entity` 기반 클래스에서 파생. ID 공간 분리 (`< NPC_ID_START`는 PC). 시야/이동 코드 1개 경로로 통합. ✅ Entity/Player 완료, NPC 스켈레톤 완료
2. **모든 비동기 작업을 IOCP로 단일화**: 패킷 IO + 타이머 + DB 콜백 모두 `PostQueuedCompletionStatus`로 워커 풀이 디스패치. ✅ IO_TIMER 완료
3. **per-Entity view_list 도입**: ✅ Stage 2 Phase C 완료
4. **Send-Lockless 패턴**: ✅ Stage 1 완료
5. **Object Pool**: ✅ OVERLAPPED_EX 풀, TimerOverlapped 풀 완료

---

## 3. 단계별 진행 상황

### ✅ Stage 1 — 안정성 핫픽스 (완료)

**원래 계획**: `unsafe_erase` → 안전 erase 패턴, `SendToSector` 락 안 send 제거, STRESS_TEST 빌드 복구.

**실제 적용**:
- `tbb::concurrent_map` → `unordered_map + shared_mutex`로 1차 교체 (이후 Stage 2에서 다시 `tbb::concurrent_hash_map`으로 변경)
- `SendToSector`, C2S_LOGIN/C2S_MOVE 핸들러 모두 **2단계 락 분리** 적용 (섹터 락에서 ID만 수집 → 락 밖에서 send)
- `STRESS_TEST/NetworkModule.cpp` 헤더 경로, 옛 패킷명(`S2C_MOVE_PLAYER` 등) → 신 패킷명 교체
- `C2S_Move` 송신을 dir 기반 → target x,y 좌표 기반으로 변경 (월드 경계 클램프)
- `MMOSERVER_Termproject.vcxproj`에서 존재하지 않는 `..\..\npc.cpp` 참조 제거
- C++20 `_SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING` 정의 추가

**검증**:
- Debug/Release × MMOSERVER/STRESS_TEST 모두 빌드 성공
- 30초 스트레스 테스트: 587~589 connects, 0 reject, 0 error, 0 stderr

---

### ✅ Stage 2 — Entity 통합 모델 + ObjectPool + view_list (완료)

#### Phase A — ObjectPool
- `Core/ObjectPool.h` 작성 (락 기반 free-list 템플릿)
- `do_send`의 `new OVERLAPPED_EX` → `g_send_pool.Acquire()`, IO_SEND 완료 시 `Release`

#### Phase B — Entity / Player
- `Core/Entity.h` 작성 (id, x, y, view_list, view_lock 포함 베이스)
- 기존 `SESSION` 구조체 → `Player : public Entity`로 리네임 + 상속
- 모든 `shared_ptr<SESSION>` → `shared_ptr<Player>` 치환

#### Phase C — view_list 차분
- `LOGIN` 핸들러: 9섹터 후보 수집 → IsInView 필터 → 초기 view_list 구축 → 상호 view 등록
- `MOVE` 핸들러: 새 위치 후보 + 기존 view_list 합집합 일괄 조회 → entered/left/stayed 차분 → 양쪽 view_list 갱신 후 적절한 Add/Move/Remove 전송
- `disconnect` 경로: view_list 스냅샷 → 시야 안 entity에서 자신 제거 + Remove 전송
- `LOGOUT` 핸들러: `closesocket`만 호출 (disconnect 경로가 정리 담당)
- **버그 수정**: 기존 9섹터 풀스캔 방식이 멀리 이동 시 old view에 있던 원거리 entity에게 Remove를 못 보내던 문제 해결

#### Phase D — NPC 스켈레톤
- `Core/NPC.h` 작성 (NpcType, NpcMoveMode, NpcFsmState 열거형 + 데이터 필드)
- **World 클래스는 Stage 4 NPC 활성화와 함께 도입 예정** (지금은 NPC 객체 없어 빈 컨테이너가 되므로 보류)

#### 추가 — concurrent_hash_map 교체
- 분석 결과 `shared_mutex + unordered_map`이 단일 글로벌 락 병목으로 작용
- `std::shared_mutex` 제거, `tbb::concurrent_hash_map<int, shared_ptr<Player>>`로 교체
- 모든 find/insert/erase 호출 사이트(11곳)를 accessor 패턴으로 재작성
- **표절 우려 검토**: TBB 자료구조 사용은 IOCP/타이머/DB 영역과 무관. 원래 코드도 `tbb::concurrent_map`을 썼음. 채점 가이드의 표절 감점 대상 아님

**검증**:
- 30초 회귀: 587~589 connects, 0 error, 0 stderr
- 단, 노이즈가 커서 동접 한계(50~120)는 측정 의미 미미. Stage 4 NPC 도입 후 view_list 효과가 가시화될 예정

---

### ✅ Stage 3 — TimerManager (완료)

**새 파일**:
- `Core/OverlappedTypes.h` — `IO_TYPE` enum (`IO_RECV/IO_SEND/IO_ACCEPT/IO_TIMER`)
- `Core/TimerManager.h` — `TimerEvent` (min-heap용 비교 반전), `TimerOverlapped`, `TimerManager` 클래스
- `Core/TimerManager.cpp` — `std::priority_queue + std::mutex + std::condition_variable` 단일 dispatcher 스레드 구현

**main cpp 통합**:
- `g_timer_pool` (TimerOverlapped 전용 풀), `g_timer_manager` 전역 추가
- `main()`에서 콜백 등록: 만기 시 `PostQueuedCompletionStatus`로 IOCP에 IO_TIMER post
- `worker_thread`에 IO_TIMER 분기를 **disconnect 체크 앞**에 배치 (bytes_transferred=0 misfire 회피)
- `--test-timer` 모드 추가: IOCP 없이 TimerManager만 검증 후 종료

**vcxproj**:
- 모든 ItemDefinitionGroup에 `/utf-8` 컴파일 플래그 추가 (한국어 주석 CP949 오인 → 컴파일 에러 방지)

**검증**:
- 1000개 이벤트, 50~5000ms 랜덤 딜레이로 schedule
- 결과: 1000/1000 발화, ordering PASS, 평균 lag 7ms, max lag 16ms, 100ms 초과 0건
- 30초 회귀: 0 error, 0 stderr

**표절 회피**:
- `std::priority_queue + std::mutex + std::condition_variable` 표준 라이브러리 조합
- TBB `concurrent_priority_queue` 미사용
- `Docs/npc.cpp:638~675` 구조 참고 X — `wait_until` + predicate 패턴은 cppreference 표준 idiom
- 변수명/메서드명 모두 자체 작성

---

### ✅ Stage 4 — NPC 200K 스폰 + AI (FSM + Roaming + Lazy) (완료)

**신규 파일**:
- `Core/NPC.h` — 기존 스켈레톤 확장: visual_id / name / hp / max_hp / level / area_x1..y2 / atomic<bool> active 필드 추가
- `Core/World.h / .cpp` — `std::unique_ptr<NPC[]> g_npcs` (NUM_NPCS=200K) 단일 할당. id↔index 변환 inline 헬퍼 (`IsNpcId`, `GetNpc`)
- `Core/NpcSpawner.h / .cpp` — `data/npc_spawn.txt` 파싱 (커스텀 라인 포맷, 가산점 +5 스크립트 NPC 배치 항목 충족)
- `data/npc_spawn.txt` — 200K NPC 기본 구성 (Peace/Roaming×3, Agro/Roaming×1)

**main.cpp 통합**:
- `SendAddNpc(player, npc)` — NPC 데이터를 S2C_AddObject 포맷으로 송신 (object_id ≥ NPC_ID_START로 클라가 NPC 식별)
- `SyncPlayerNpcView(player)` — 9섹터 NPC 후보 → IsInView 필터 → 플레이어 view_list와 차분 → entered/left만 처리. entered NPC는 `compare_exchange_strong(false, true)`로 활성화 + NPC_MOVE 첫 타이머 등록
- `NpcOnMove(npc_id)` — Roaming: rng 0~7 (4방향+50% 정지). Agro 타입: 시야 안 가장 가까운 player를 1축씩 1칸 추적 (Chasing, target_id 갱신). 추적 대상 없으면 Roaming으로 폴백. → area+월드 경계 클램프 → UpdateObjectSector → 시야 차분 → entered Add / left Remove / stayed Move 송신 → view 비었으면 active=false (재스케줄 안 함), 아니면 NPC_MOVE_INTERVAL 재스케줄
- `worker_thread::IO_TIMER` 분기에 TimerEventKind 디스패치 추가
- LOGIN 핸들러: 기존 player view 구축 직후 `SyncPlayerNpcView` 호출
- MOVE 핸들러: player diff에서 NPC ID는 제외 (view_list 보존), 끝에 `SyncPlayerNpcView` 호출
- disconnect 경로: viewer 중 NPC는 `n.view_list.erase(player_id)`만, Player는 기존대로 Remove 패킷 송신
- main(): InitWorld(NUM_NPCS) → LoadNpcSpawnScript (CWD/`../../`/`../../../` 경로 후보) → 각 NPC를 UpdateObjectSector로 등록

**Lazy AI 정합성 (race-free 설계)**:
- 활성화: 플레이어가 NPC view에 진입 → 락 안에서 `npc.view_list.insert(pid)` → 락 밖에서 `active.compare_exchange_strong(false, true)` → 첫 타이머 등록
- 비활성화: NpcOnMove 끝부분에서 `lock(npc.view_lock)` 안에 `view_list.empty()` 체크 후 `active=false`. lock 밖에서 reschedule
- 양방향 시나리오 4가지 (AI/Player 락 순서 교환 + active 상태 교환) 모두 안전: 활성/비활성 일관성 보장됨

**검증**:
- Release x64 / 30초 스트레스 (echo "" | STRESS_TEST)
- 결과: Connect 1,092 / Login 1,092 / Disconnect 962 / **Error 0 / Reject 0**
- 200K NPC 활성 상태에서도 처리량이 Stage 3(589 connect/30s) 대비 약 1.85배 향상
- NPC 200K 메모리 풋프린트: ~25MB (unique_ptr<NPC[]>, 각 NPC ~130B)

**표절 회피**:
- AI/시야 구조 모두 직접 작성, `Docs/npc.cpp`에서 변수명/함수명 모두 다름
- NpcSpawner는 표준 sscanf 기반의 라인 포맷 (외부 라이브러리 0)
- thread_local std::mt19937 사용 (cppreference 표준 idiom)

---

### ✅ Stage 4.5 — Lua 5.5 스크립트 엔진 연동 (완료)

**도입 패키지**: `lua` 5.5.0 (NuGet, coapp/grottel) — `LuaType=static`으로 lua_static.lib 링크. v143 x64 Release/Debug 모두 지원. DLL 배포 불필요.

**신규 파일**:
- `Core/LuaVM.h / .cpp` — `lua_State*` 래퍼. `OpenStdLibs / DoFile / CallGlobalVoid` API. 헤더에서 `lua.h` 노출 회피 위해 내부 `void*` 보관
- `data/npc_ai.lua` — 검증용 hello world 스크립트. 추후 OnTick/OnAggro/OnHit 콜백 정의 예정

**vcxproj 통합**:
- `packages.config`에 `<package id="lua" version="5.5.0" />` 추가
- `<LuaType>static</LuaType>` 설정 (PropertyGroup Label="UserMacros")
- `ImportGroup Label="ExtensionTargets"`에 `Lua.targets` import
- `EnsureNuGetPackageBuildImports`에 lua targets 존재 검사 추가

**main.cpp 통합**:
- 전역 `LuaVM g_lua;`
- main(): NPC 스폰 직후 `g_lua.OpenStdLibs()` → `DoFile("data/npc_ai.lua")` → `CallGlobalVoid("Hello")` 검증 (실패 시 에러 로그)

**검증**:
- Release x64 빌드 성공 (lua_static.lib 자동 링크)
- 부팅 로그: `[lua] npc_ai.lua loaded successfully (Lua Lua 5.5)` / `[lua] Hello() called from C++. Math.pi sanity check: 3.1415926535897931`

**다음 단계 설계 노트 (Stage 5에서 결정)**:
- 스레드 모델: 현재 단일 `lua_State`. 200K NPC 환경에서 호출 빈도 높아지면 worker별 state 풀로 전환 검토
- C↔Lua 호출 시그니처: `OnTick(npc_id) → (action, dx, dy)` 같은 형태로 NPC AI를 점진적 이관 가능
- 표절 영향: Lua 자체는 MIT 라이센스의 공인 인터프리터이며 IOCP/Timer/DB 본체 코드와 무관 → 감점 대상 아님

---

### ✅ Stage 4.6 — NPC AI Lua 이관 + PDF 스펙 정합 (완료)

PDF 기본 스펙을 코드/스크립트로 모두 반영. AI 결정 로직은 C++ → Lua로 완전 이관.

**PDF 스펙 매핑**:

| PDF 항목 | 구현 위치 | 상태 |
|---|---|---|
| 0.5초/칸 이동 (몬스터/플레이어) | `GameConfig.h: NPC_TICK_INTERVAL_MS=500` | ✅ |
| Peace: 때리기 전까지 가만히 | `npc_ai.lua: OnTick`에서 type=Peace + target_id=-1이면 정지 | ✅ (피격 시 Agro 전환은 Stage 5 전투에서) |
| Agro: 11x11에 접근 시 추적 | `AGRO_DETECT_RANGE=5`, `nearest_dist <= AGRO_RANGE` 체크 | ✅ |
| 고정 모드: 정지 | `OnTick`에서 MOVE_FIXED + 비추적이면 (0,0,-1) | ✅ |
| 로밍 모드: 스폰 중심 20x20 | `ROAM_AREA_RANGE=10`, `roaming_step`에서 `spawn ± ROAM_RANGE` 클램프 | ✅ |
| 처음 인식한 공격 대상 계속 추적 | `target_id` 한번 설정되면 시야 안 있는 한 그 대상의 좌표로 step_toward | ✅ |
| 추적 대상 시야 이탈 → 해제 | `target_x == -1`(C++가 시야 lookup 실패) 시 Lua가 -1 반환 | ✅ |
| 모든 정보 Script 저장 | `data/npc_spawn.txt` (배치) + `data/npc_ai.lua` (AI) | ✅ (+5 가산점) |
| 사망 후 30초 부활 | `NPC_RESPAWN_MS=30000` 상수 정의 (Stage 5에서 실제 사용) | 상수만 |
| HP/EXP/A* | Stage 5/6에서 | 미구현 |

**신규 / 변경 파일**:
- `Core/GameConfig.h` (신규) — `NPC_TICK_INTERVAL_MS`, `AGRO_DETECT_RANGE=5`, `ROAM_AREA_RANGE=10`, `NPC_RESPAWN_MS=30000` 등 PDF 스펙 상수 모음. `protocol_2026.h`는 건드리지 않고 게임 규칙만 분리
- `Core/LuaVM.h/cpp` 확장 — `NpcTickContext / NpcTickResult` 구조체, `NpcTick(ctx, out)` 스레드안전 호출 (내부 mutex), `SetGlobalInt(name, value)`로 enum/상수 노출
- `data/npc_ai.lua` 재작성 — `OnTick(ctx) -> (dx, dy, new_target_id)` 함수. 추적 우선순위: ① 현재 target 추적 → ② Agro 11x11 트리거 → ③ Roaming 또는 Fixed
- `MMOSERVER_Termproject.cpp`:
  - `NpcOnMove`를 Lua 호출 기반으로 통째 교체. C++는 view_list 스냅샷 + 가장 가까운 player(chebyshev) 계산 + target 좌표 lookup만 하고, 이동/추적 결정은 Lua에 위임. 결과를 받아 sector/view 동기화 + 패킷 송신
  - `main()` 부팅 시퀀스에 `SetGlobalInt`로 `TYPE_PEACE/TYPE_AGRO/MOVE_FIXED/MOVE_ROAMING/AGRO_RANGE/ROAM_RANGE` 노출
  - 모든 `NPC_MOVE_INTERVAL` 사용처를 `NPC_TICK_INTERVAL_MS`로 교체 (3곳)

**LuaVM 스레드 모델**:
- 단일 `lua_State` + `std::mutex` 직렬화. 200K NPC가 worker 풀에서 OnTick을 동시에 호출해도 안전
- 호출 실패 시 안전 디폴트(정지 + target 유지)로 폴백 → AI 버그가 NPC 정적 상태로만 나타나고 서버는 죽지 않음

**검증 (Release x64, 30초 스트레스, NPC 200K 활성)**:
- Boot: `[Lua] npc_ai.lua loaded. AGRO_RANGE=5 ROAM_RANGE=10 TICK=500ms`
- Connect 560 / Login 560 / Disconnect 474 / **Error 0 / Reject 0**
- Lua mutex 직렬화 영향으로 throughput은 Stage 4 1차 검증(1,092) 대비 절반 수준이지만 안정성 유지. tick interval이 500ms로 절반 줄어 NPC tick 호출 빈도도 2배 → Lua call ~2x 증가가 주 원인
- 향후 worker별 state 풀 또는 Lua 결정을 batch로 묶기 등으로 회복 가능

**스펙 누락 항목 (Stage 5/6에서 처리)**:
- 사망 후 30초 부활, HP 5초마다 10% 회복, EXP 공식(level²×2, Agro/Roaming ×2배), 사망 시 EXP 50% 감소
- A* 길찾기 (장애물 회피)
- 전투 (A키 → 인접 4타일, 1초/공격), 전투 메시지
- DB 저장
- 채팅

---

### ✅ Stage 5 — 전투 + HP/EXP + 사망/리스폰 + 채팅 (완료)

**프로토콜 5종 추가** (`protocol_2026.h`):
- `S2C_ATTACK_ANIM` (브로드캐스트 — 공격자/방향)
- `S2C_DAMAGE` (공격자/대상/데미지/대상 새 HP/대상 좌표)
- `S2C_DEATH` (사망 객체/위치)
- `S2C_RESPAWN` (부활 객체/위치/HP)
- `S2C_LEVEL_UP` (객체/새 레벨/새 max_hp)

**서버 (`MMOSERVER_Termproject.cpp` + `Core/Entity.h` + `Core/NPC.h` + `Core/GameConfig.h`)**:
- `Entity::direction` (atomic) — MOVE/TELEPORT에서 dx/dy로 갱신, 공격 방향에 사용
- `Player`: hp/max_hp/exp/level/last_attack_ms (모두 atomic, 기본 100/100/0/1/0)
- `NPC::last_attack_ms` (atomic) — NPC 공격 쿨타임용
- `GameConfig`: `ATTACK_INTERVAL_MS=1000`, `BASE_DAMAGE_PER_LEVEL=10`, `NPC_ATTACK_INTERVAL_MS=1000`, `NPC_BASE_DAMAGE=5`, `PLAYER_SPAWN_X/Y=1000`
- `C2S_ATTACK` 핸들러: 쿨타임 → `S2C_ATTACK_ANIM` 브로드캐스트 → 인접 4타일 NPC 데미지 → `S2C_DAMAGE` 브로드캐스트 → 사망 시 `S2C_DEATH` + 30s 리스폰 예약 + EXP 부여 + 레벨업 → `S2C_LEVEL_UP` + `S2C_StatusChange`
- `NpcOnRespawn(npc_id)`: HP/위치/state 복구, 섹터 재등록, 시야 내 플레이어 `SendAddNpc` + `S2C_RESPAWN`, Lazy AI 재시작
- `NpcOnMove`에 카디널 인접 공격 분기 추가: target이 manhattan 1이면 데미지 → 플레이어 HP=0 시 `PlayerOnDeath`
- `PlayerOnDeath(session)`: 시야 정리 → `S2C_DEATH` → EXP -50% → HP=max → (1000,1000)로 텔레포트 → 새 시야 구축 → `S2C_RESPAWN` + `S2C_StatusChange`
- `PlayerOnHpRegen(client_id)`: 5초마다 max_hp의 10% 회복 + `S2C_StatusChange` → 재스케줄. 로그인 시 첫 등록
- EXP 공식: `npc.level² × 2 × (Agro? ×2) × (Roaming? ×2)` — 레벨업 임계: `level² × 2`
- `C2S_CHAT` 핸들러: 시야 내 플레이어에게 `S2C_CHAT_MESSAGE` 브로드캐스트
- TimerEventKind 디스패치 확장: NpcRespawn / HpRegen

**클라이언트 (`CLIENT/client_sample/client.cpp`)**:
- `OBJECT` 클래스 확장: walk + attack 양쪽 시트 지원 (`m_walk_tex/m_attack_tex`), `m_attacking/m_attack_dir/m_attack_clock`, `on_attack(direction)` 메서드
- `S2C_ATTACK_ANIM`: 공격자 위치에 slash 이펙트 + `on_attack` 호출
- `S2C_DAMAGE`: 대상 위치에 blood 이펙트 + (자기 자신이면) HP HUD 갱신
- `S2C_DEATH`: death 위치에 soul 이펙트 + 자기는 hide / 다른 엔티티는 erase
- `S2C_RESPAWN`: respawn 위치에 pillar 이펙트 + (자기 자신이면) 위치/HP/카메라 갱신 + show
- `S2C_LEVEL_UP`: 객체 위치에 levelup-burst 이펙트 + (자기 자신이면) level/max_hp 갱신
- `S2C_StatusChange`: HP/EXP/Level HUD 자동 반영
- `S2C_CHAT_MESSAGE`: 발신자 이름 + 메시지 형식으로 g_chat_log에 추가 (최근 6개 유지)
- Enter 토글: **폴링 기반 엣지 검출**(`sf::Keyboard::isKeyPressed` rising edge) — 키 자동반복/한국어 IME 환경에서도 안정 작동
- TextEntered로 ASCII 32~126 + BackSpace 입력 처리
- HUD 그리기: 채팅 패널 + 메시지 6줄 + (채팅 모드 시) 입력 박스 + 깜빡이는 커서

**리소스 통합** (`Resource/`):
- `monsters/red-orc-walk-256-4dir.png` — Agro NPC 시각화 (체스 말 교체)
- `hero/hero-attack-192x256-4dir.png` — 공격 모션 시트
- `effects/` — blood, soul-death, respawn-pillar, level-up-burst, slash 시트 5종
- `ui/` — HP/MP 구슬 프레임, EXP 바 프레임/fill, 채팅 패널
- 클라 폴더 구조 + 명명 규칙 정립 (`Resource/README.md`)

**버그 수정 (Stage 5 도중)**:
- **클라 `process_data` signed char 버그**: `char ptr[0]`이 128 이상이면 음수로 잡혀 `size_t`에 거대 양수로 들어감 → S2C_ChatMessage(209B) 같은 큰 패킷이 영원히 미처리되며 후속 패킷도 함께 누적 정지. `static_cast<unsigned char>(ptr[0])`로 수정
- **NPC가 플레이어 타일 침범**: `npc_ai.lua`의 `step_toward`가 다음 칸이 타겟 좌표와 일치하면 정지하도록 `would_step_onto` 가드 추가 → 카디널 인접 1칸 거리 유지 → 공격 4타일 판정 정상화
- **STRESS_TEST 프로토콜 sync**: 새 5종 패킷 no-op 케이스 추가 + `S2C_LOGIN_RESULT` fall-through bug 수정 + 알 수 없는 패킷의 MessageBox+무한루프 제거

**검증 (2026-05-22, Release x64, 30초 스트레스, NPC 200K 활성)**:
- Connect 588 / Login 588 / Disconnect 520 / **Error 0 / Reject 0**
- stderr 0 byte
- Stage 4(589 connect) 대비 동등한 성능, 신규 시스템(이펙트/HUD/사망부활/채팅) 안정 동작
- 클라 수동 검증: 워리어/오크 시각, walk/attack 애니, HP/MP/EXP HUD 게이지, 사망 30초 리스폰, 채팅 시야 브로드캐스트 모두 정상

---

### ✅ Stage 6.1 — 장애물 맵 (완료, 2026-05-24)

**신규 파일**:
- `Core/Map.h` — `Map::g_obstacle_bits[BITMAP_BYTES]` (1bit/tile, 500KB), `IsBlocked(x,y) / IsWalkable(x,y)` inline, `InBounds(x,y)`. 월드 밖은 항상 blocked로 취급
- `Core/Map.cpp` — `LoadObstacles(path)`: `rect x1 y1 x2 y2` 라인 파싱 → 반열림 구간 [x1,x2)×[y1,y2) 마킹. 적용된 rect 수 반환
- `data/obstacles.txt` — 64개 rect (NW 균열석 / NE 벽돌 마을 / SW 혈흔 미로 / SE 룬 서클 컨셉). 207,100 타일 = 5.18% 점유. PLAYER_SPAWN(1000,1000) ±50, 4지역 경계 ±20 통로 안전 지대 확보

**통합**:
- `main()` 부팅 시퀀스: NPC 스폰 *직전*에 `Map::LoadObstacles` 호출 (NpcSpawner가 IsBlocked 회피 가능하도록)
- `MMOSERVER_Termproject.vcxproj`: ClCompile/ClInclude에 Map.cpp/.h 등록
- `C2S_MOVE / C2S_TELEPORT` 핸들러: 클램프 직후 `if (Map::IsBlocked(new_x, new_y)) break;` — 장애물 칸으로의 이동/텔레포트 거부
- `NpcOnMove` (Lua dx/dy 적용 후): 새 좌표 IsBlocked면 old로 되돌리고 stay (A* 도입 전 임시 정책 — 사용자 합의)
- `NpcSpawner::LoadNpcSpawnScript`: 무작위 좌표 추첨을 최대 32회 재시도, fallback으로 area 선형 스캔. 모든 칸이 막혀있으면 슬롯 스킵
- `LOGIN` 핸들러: `rand() % WORLD_*` 좌표를 32회 추첨해 walkable 보장, 실패 시 PLAYER_SPAWN(1000,1000) 폴백

**검증 (Release x64, 30초 stress, NPC 200K 활성, 2026-05-24)**:
- Boot: `[Map] Loaded 64 rects, 207100 blocked tiles (5.1775% of world)`
- `[NpcSpawner] Spawned 200000 NPCs` — 한 슬롯도 빠짐없이 walkable 위치 확보
- 로그인 좌표 샘플: (41,467) (1169,1724) (1281,827) (1961,491) (995,1942) — 모두 walkable 영역
- 30초간 약 580+ connect, 신규 stderr 0건 (기존 npc_spawn 경로 후보 탐색 메시지 2줄만 — Stage 5 회귀와 동일)

**표절 회피**:
- 비트맵 + IsBlocked는 표준 비트 시프트만 사용 (`Docs/npc.cpp`에 동등 코드 없음)
- LoadObstacles는 표준 sscanf 기반의 라인 파서 (NpcSpawner와 동일 패턴 — 자체 작성)

---

### ✅ Stage 6.2 — A* 길찾기 (완료, 2026-05-24)

**신규 파일**:
- `Core/AStar.h` — `AStar::AStarStep(sx, sy, gx, gy, dx, dy)` 단일 API. 첫 step만 4방향(dx+dy 절댓값 합 1)으로 반환
- `Core/AStar.cpp` — `std::priority_queue + unordered_map<key,g>` 표준 A*. manhattan heuristic, 4방향 이웃, key = `x * Map::H + y`
  - 검색 영역: (start, goal) bbox + margin 8 (~17x17 워크스페이스)
  - 노드 상한 `MAX_NODES = 256` — 200K NPC가 동시에 막혀도 cataclysm 방지
  - 목표 칸 자체는 walkable 여부 무관 (플레이어가 서 있을 수 있음)
  - stale entry는 `cur.g > g_score[ck]` 체크로 스킵

**통합 (`MMOSERVER_Termproject.cpp` NpcOnMove)**:
- Lua가 반환한 (dx, dy)가 막힌 칸을 가리키면:
  - `new_target != -1` (추적 중)이면 A* 호출 → 첫 step 채택
  - 채택 조건: 스폰 박스 (±ROAM_AREA_RANGE) 내 + 재확인 IsBlocked == false
  - 그 외 (로밍 중 또는 A* 실패) 기존 stay 정책 유지
- 호출 빈도: NPC tick의 일부만 막힘 + 추적 중인 NPC만 → 200K 전체 중 미미한 비율

**검증 (Release x64, 30초 stress, NPC 200K 활성, 2026-05-24)**:
- 1,767 라인 stdout / 신규 stderr 0건 / 처리량 Stage 6.1(1,764)과 동등
- A* 노드 한도(256) + bbox(17×17) 조합으로 worst case 호출 비용 무시 가능

**표절 회피**:
- `std::priority_queue + unordered_map` 표준 라이브러리 조합만 사용
- 격자 A*는 cppreference/Wikipedia 일반 idiom — `Docs/npc.cpp`엔 길찾기 코드 자체가 없음

---

### ✅ Stage 6.3 — DB 영속성 (완료, 2026-05-24)

**신규 파일**:
- `Core/Db/DbTypes.h` — `PlayerSnapshot`(username/hp/max_hp/exp/level/x/y), `DbRequest/Response`, `DbReqKind` enum
- `Core/Db/IDbBackend.h` — `Load(username, out)` / `Save(snap)` 추상 인터페이스. 첫 구현은 JSON 파일, 추후 ODBC 백엔드 교체 가능
- `Core/Db/JsonFileBackend.h/.cpp` — 한 유저당 한 JSON 파일(`<root>/<username>.json`). 외부 JSON 라이브러리 없이 자체 minimal 파서(키 검색 + sscanf). 안전한 username sanitize (영숫자/언더스코어만, 그 외 `_`로 치환). atomic write (`.tmp` → rename)
- `Core/Db/DbWorker.h/.cpp` — `std::queue + mutex + condition_variable` 기반 단일 워커 스레드. `EnqueueLoad/EnqueueSave` API. 완료 시 외부 콜백 호출(콜백이 `PostQueuedCompletionStatus`로 `IO_DB_DONE` 이벤트 post)
- `data/players/` — JSON 저장 디렉토리 (백엔드 ctor가 mkdir)

**통합**:
- `OverlappedTypes.h`: `IO_DB_DONE` 추가
- `TimerManager.h`: `TimerEventKind::PlayerAutoSave` 추가
- `GameConfig.h`: `PLAYER_AUTO_SAVE_INTERVAL_MS = 30000`
- `MMOSERVER_Termproject.cpp`:
  - 전역 `DbWorker g_db_worker;` + `DbOverlapped` 구조체 + `ObjectPool<DbOverlapped> g_db_pool;`
  - main(): `data/`(또는 `../../data/`, `../../../data/`) 위치 자동 탐색 → `JsonFileBackend(<root>/players)` 으로 DbWorker.Start. 첫 PlayerAutoSave 타이머 30초 후 등록. 종료 시 `g_db_worker.Stop()` 호출하여 워커 스레드 join
  - worker_thread: IO_TIMER 분기에 `PlayerAutoSave` 추가 + 신규 분기 `IO_DB_DONE` (DbOverlapped → DbResponse 이동 → 풀로 회수 → OnDbResponse 디스패치)
  - LOGIN 핸들러: 기존 spawn 코드 제거, `g_db_worker.EnqueueLoad(client_id, username)` 한 줄로 단순화
  - 신규 함수 `OnPlayerSpawn(client_id, snap, exists)`: 기존 LOGIN의 spawn~view~HpRegen 코드를 옮김 + DB에서 복원한 hp/max_hp/exp/level 적용. 저장 좌표가 장애물/월드 밖이면 PLAYER_SPAWN(1000,1000) 폴백
  - 신규 함수 `OnDbResponse(resp)`: Load → OnPlayerSpawn, Save → stderr에 실패만 로깅
  - 신규 함수 `PlayerOnAutoSave()`: g_clients 스냅샷 → 각자 EnqueueSave → 30초 재스케줄
  - 신규 함수 `SnapshotPlayer(session)`: atomic 필드 → PlayerSnapshot 캡쳐
  - disconnect 경로: 이름 있는 클라는 closesocket 직전에 EnqueueSave

**검증 (Release x64, 30초 stress, NPC 200K 활성, 2026-05-24)**:
- Boot: `[Db] JSON backend root: ../../../data/players`
- LOGIN: `[Login] Client 0 (new) as 1 at (41, 468) hp=100 lv=1` — 비동기 DB Load 응답 도착 후 spawn 처리
- **30초간 586개 JSON 파일 생성** (`data/players/<n>.json`)
- 샘플 1.json: `{"username":"1","hp":100,"max_hp":100,"exp":0,"level":1,"x":1000,"y":1000}` — PlayerOnDeath 리스폰(1000,1000) 후 disconnect된 케이스
- 처리량 1,765줄 / 신규 stderr 0건 (Stage 6.2와 동등)

**표절 회피**:
- ODBC/실습 DB 코드 미사용. JSON 자체 minimal 파서로 자체 구현 — `Docs/npc.cpp`와 무관
- 비동기 모델: `std::condition_variable + queue` + `PostQueuedCompletionStatus`로 IOCP 통합. 표준 idiom

**SQL Server 백엔드 확장 (선택사항)**:
- `Core/Db/OdbcBackend.h/.cpp` 추가 + `main()`에서 `JsonFileBackend` 대신 새 백엔드 주입만 하면 교체 완료 (IDbBackend 인터페이스 그대로)
- 채점 환경에 SQL Server가 없을 경우 JSON stub만으로도 "비동기 DB 워커 큐 + 영속성" 명세는 충족

---

### ✅ Stage 7.1 — 스킬 시스템 (완료, 2026-05-25)

**신규 패킷 2종** (`protocol_2026.h`):
- `C2S_UseSkill` (skill_id: 1=AoE, 2=Line, 3=Heal)
- `S2C_SkillEffect` (object_id, skill_id, x, y)

**서버 (`MMOSERVER_Termproject.cpp` + `Core/GameConfig.h`)**:
- `Player`: `last_skill1_ms / last_skill2_ms / last_skill3_ms` atomic 쿨타임 필드 추가
- `C2S_USE_SKILL` 핸들러:
  - 스킬 1 AoE: 시전자 중심 반경 3칸(chebyshev) 내 모든 NPC에 `level*15` 데미지. 사망/EXP/레벨업까지 C2S_ATTACK와 동일 플로우. 쿨 3초.
  - 스킬 2 Line: 현재 방향 직선 5칸의 모든 NPC에 `level*20` 데미지. 쿨 3초.
  - 스킬 3 Heal: 자신 HP를 `max_hp*30%` 회복 후 StatusChange 전송. 쿨 10초.
  - 성공 시 시야 내 플레이어 전원에게 `S2C_SkillEffect` 브로드캐스트
- `GameConfig.h`: `SKILL_AOE_COOLDOWN_MS`, `SKILL_LINE_COOLDOWN_MS`, `SKILL_HEAL_COOLDOWN_MS`, `SKILL_AOE_RANGE=3`, `SKILL_LINE_RANGE=5`, `SKILL_AOE_DAMAGE_PER_LEVEL=15`, `SKILL_LINE_DAMAGE_PER_LEVEL=20`, `SKILL_HEAL_PERCENT=30`

**클라이언트 (`client.cpp`)**:
- Q=AoE / W=Line / E=Heal 키 입력 → `C2S_UseSkill` 전송 + 클라 쿨타임 시계 시작
- `S2C_SkillEffect` 수신 시 스킬별 이펙트 재생 (AoE=levelup_tex, Line=slash_tex, Heal=respawn_tex 재활용)
- 스킬 슬롯 HUD 3칸 (화면 하단 EXP 바 위에): 쿨타임 오버레이 + 남은 초 표시

**STRESS_TEST**: `S2C_SKILL_EFFECT` no-op 케이스 추가

**검증**: 서버/클라/스트레스 모두 Release x64 0 에러 빌드 성공

---

### Stage 7 — 가산점 추가 요소 진행 현황 — *게임성 40점이 최종 점수 가른다*

| 우선순위 | 항목 | 점수 | 상태 |
|---|---|---|---|
| A | 스크립트 NPC 배치 | 5점 | ✅ 완료 (Stage 4 — `npc_spawn.txt`/`npc_ai.lua`) |
| B | 스킬: 범위/방향성 | 10점 | ✅ 완료 (Stage 7.1 — Q/W/E AoE·Line·Heal) |
| C | 보스 패턴 (이동/채팅/스킬) | 5-10점 | ✅ 완료 (Stage 7.2 — 바이옴별 보스 4마리, 2단계 AI) |
| D | 파티 시스템 | 10점 | ✅ 완료 (Stage 7.4 — 초대/EXP분배/미니맵/HP바) |
| E | 아이템: 소모 + 누적 + 인벤 + 장착 | 20점 | ✅ 완료 (Stage 8 — 4요소 모두 구현) |
| F | 퀘스트 풀세트 | 25점 | ✅ 완료 (Stage 9 — 슬레이/대화/연쇄 데이터 주도) |

**전략**: A→B→C→D→E→F 가산점 풀세트 확보 완료. 남은 작업은 (선택) SQL Server 백엔드 / 부하 측정 강화뿐.

---

### ✅ Stage 7.2 — 보스 패턴 (완료, 2026-05-25)

**설계**:
- 바이옴별 보스 1마리씩 총 4마리 (GrassBoss/ForestBoss/DesertBoss/IceBoss)
- Level 50, HP 5000. 스폰 위치 Fixed (죽어도 제자리 부활)
- Agro 감지 반경 10칸 (일반 NPC의 2배)

**보스 AI (Lua `OnBossTick`)**:
- Phase 1 (HP > 50%): 광범위 Agro 추적 + 20틱마다 도발 채팅
- Phase 2 (HP ≤ 50%): 분노 선언 + 3틱마다 채팅 교대 출력
- 5틱마다 반경 3칸 AoE 데미지 (BOSS_BASE_DAMAGE=30)
- 비전투 시 스폰 위치로 복귀 이동

**서버 처리**:
- `NpcOnMove`에서 Boss 타입 분기 → `BossTick` 호출
- `chat_id` 결과 → `S2C_CHAT_MESSAGE` 시야 내 전원 브로드캐스트
- `do_boss_aoe` 결과 → 반경 내 플레이어에 데미지 + `S2C_DAMAGE`
- 사망 리스폰: 5분 (`BOSS_RESPAWN_MS=300000`)
- EXP: 일반 공식 × 20배 (`BOSS_EXP_MULTIPLIER=20`)

**변경 파일**:
- `Core/NPC.h` — `NpcType::Boss` 추가, `boss_tick_count` atomic 필드
- `Core/GameConfig.h` — BOSS_* 상수 9종
- `Core/LuaVM.h/.cpp` — ctx 확장(hp/max_hp/boss_tick_count), result 확장(chat_id/do_boss_aoe), `BossTick()` 메서드
- `Core/NpcSpawner.cpp` — Boss 파싱, 보스 스탯 초기화
- `data/npc_spawn.txt` — 4마리 보스 추가 (총 200000 유지)
- `data/npc_ai.lua` — `OnBossTick()` 함수 (2단계 AI)
- `MMOSERVER_Termproject.cpp` — NpcOnMove 보스 분기, AoE/채팅, 사망 리스폰/EXP

---

### ✅ Stage 7.3 — 클라이언트 이펙트 완성도 강화 (완료, 2026-05-28)

게임성 점수 + 시각적 임팩트 강화를 위해 클라 이펙트 시스템을 다음 5가지 축으로 보강. 모든 변경은 `CLIENT/client_sample/client.cpp`에 집중되며, 서버는 `GameConfig.h`의 SKILL_AOE_RANGE 1개만 변경.

**1) 공격 이펙트 분리 (플레이어 vs NPC)**:
- 기존: `spawn_effect_slash(wx, wy)`가 플레이어/NPC 구분 없이 같은 슬래시 시트로 시전자 자기 자리에 그림
- 신규 텍스처 (DCSS effect/에서 추출 후 PowerShell System.Drawing으로 시트 생성):
  - `Resource/effects/attack-player-zap-128x32.png` — zap_0~3 (4프레임 32×32) 가로 합본
  - `Resource/effects/attack-npc-sandblast-96x32.png` — sandblast_0~2 (3프레임 32×32) 가로 합본
- `spawn_effect_attack_player(wx, wy)`: 시전자 4방향 인접 1칸(상/하/좌/우) **동시 spawn** — 광역 공격 시각화
- `spawn_effect_attack_npc(wx, wy, dir)`: 공격 방향 1칸**에만** spawn (dir 0=Down/1=Left/2=Right/3=Up)
- `S2C_ATTACK_ANIM` 핸들러에서 `aid >= NPC_ID_START`로 분기. NPC도 ATTACK_ANIM에 direction 동봉되어 있음 (`protocol_2026.h:155`)

**2) 데미지 숫자 floating popup**:
- `FloatingDamage` 구조체 + `g_dmg_popups` 벡터. duration 900ms
- 색상: 내가 입은 데미지 = 빨강 `(255,80,80)`, 내가 준 = 노랑 `(255,220,80)`, 제3자 = 회색 `(220,220,220)`
- 위로 떠오름(최대 -48px) + 60% 이후 알파 페이드아웃
- 동시 데미지 겹침 방지: `jitter_x = rand() % 33 - 16`
- `S2C_DAMAGE` 핸들러에서 `spawn_damage_popup(target_x, target_y, damage, color)` 호출

**3) 화면 흔들기 (Screen Shake)**:
- `g_shake_strength_px`, `g_shake_duration_ms`, `g_shake_clock` + `trigger_shake(strength, dur)` API
- 감쇠: 선형(1.0 → 0.0) × 다중 sin/cos 진동 (주파수 0.071/0.137/0.083/0.151)
- 더 강한 흔들기로 덮어쓰기 (약한 잔흔들기로 약화되지 않음)
- 트리거 지점:
  - 내가 데미지 받음: `strength = min(20, 2 + dmg*0.4)`, `dur = min(500, 150 + dmg*5)` — 보스 30데미지는 자연스럽게 강한 흔들림
  - 내가 죽음: `trigger_shake(18.0f, 600)` (S2C_DEATH)
- 통합 방식: `sf::View`로 게임 월드만 흔들고 HUD 직전에 `setView(default_view)`로 복원 — HUD/미니맵은 흔들리지 않음

**4) HP/EXP 게이지 보간 + 저체력 경고**:
- `draw_hud()` 내 `static displayed_hp_ratio / displayed_exp_ratio` 보간 상태
- 매 프레임 `displayed += (target - displayed) * 0.18` lerp (60fps 기준 ~150ms에 90% 도달)
- 잔류 0.002 미만이면 target에 스냅 (떠다니는 오래된 잔류 방지)
- HP < 30% 시: 빨강 게이지가 sin 기반 0.5초 주기로 `(180,30,30) ↔ (255,90,90)` 밝게 깜빡임. 사망 상태(`hp == 0`)는 제외

**5) Effect start_delay_ms + Q스킬 폭발 퍼짐**:
- `Effect` 구조체에 `start_delay_ms` 필드 추가. `active_ms() = clock.ms - start_delay_ms`로 통합 계산. 음수면 draw skip + is_done 미만으로 처리 — 별도 pending queue 없이 1개 큐로 spawn 시점부터 지연 가능
- 서버 `SKILL_AOE_RANGE` 3 → 2 (chebyshev 2 = 시전자 중심 5×5 = 25타일). 데미지/쿨타임은 그대로 (`level*15` / 3초)
- `spawn_effect_skill_aoe`를 십자형 17타일 → 5×5 전체 25타일 채움으로 재작성
- 폭발 퍼짐: 중심부터 외곽으로 단계별 spawn
  - cheb 0 (1타일): delay 0ms, duration 700ms
  - cheb 1 (8타일): delay 100ms, duration 600ms
  - cheb 2 (16타일): delay 200ms, duration 500ms
- 결과: 시전 시점에 중심부터 ka-pow 하고 0.2초에 걸쳐 가장자리로 퍼지며 잔불 사라짐

**검증**:
- Release x64 서버/클라 모두 빌드 성공 (C4819 워닝만 — 한글 주석 인코딩)
- 30초 stress 회귀는 별도 수행 안 함 — 클라 전용 변경이라 서버 부하 영향 없음
- 수동 검증 권장: 워리어로 A공격 시 4방향 zap, NPC가 공격 시 방향 1칸 sandblast, 데미지 숫자 색 구분, 보스 만나서 강한 흔들기 + HP 30%에서 깜빡임, Q 스킬 5×5 폭발 퍼짐

**변경 파일**:
- `MMOSERVER_Termproject/.../Core/GameConfig.h` — `SKILL_AOE_RANGE 3 → 2`
- `CLIENT/client_sample/client.cpp` — Effect 구조체 확장, 신규 함수 7개 (`trigger_shake / get_shake_offset / spawn_damage_popup / spawn_effect_attack_player / spawn_effect_attack_npc + FloatingDamage`), HUD 보간 + sin 깜빡임 로직, S2C_ATTACK_ANIM/DAMAGE/DEATH 핸들러 갱신
- `Resource/effects/attack-player-zap-128x32.png`, `attack-npc-sandblast-96x32.png` — 신규 시트 (PowerShell로 합본)

**잔존 자산 (안 쓰지만 보존)**:
- `slash_tex` / `spawn_effect_slash` — 호출처 없어졌지만 텍스처/함수는 남겨둠. 추후 크리티컬 히트나 보스 처치 연출에 재활용 가능

---

### ✅ Stage 7.4 — 파티 시스템 (완료, 2026-05-28)

게임성 가산점(10점) 항목. NPC/맵 추가 작업 없이 패킷 + 로직만으로 구현. 채팅 명령 기반 UX.

**신규 패킷** (`protocol_2026.h`):
- `C2S_PartyInvite` (target_name) — 이름으로 파티 초대
- `C2S_PartyAccept` / `C2S_PartyReject` / `C2S_PartyLeave` (헤더만)
- `S2C_PartyInvited` (inviter_id, inviter_name) — 초대 수신 알림
- `S2C_PartyUpdate` (state, member_id, member_name) — state: 0=joined / 1=left / 2=disbanded

**서버 (`MMOSERVER_Termproject.cpp`)**:
- `Player`: `atomic<int> party_id{-1}` (-1 = 파티 없음)
- `struct Party { int leader_id; vector<int> members; }` + `g_parties` (`unordered_map<int, shared_ptr<Party>>`) + `g_party_mutex` + `g_next_party_id` + `g_pending_invites` (초대받은자→초대자 매핑). `MAX_PARTY_SIZE = 4`
- `C2S_PARTY_INVITE` 핸들러: 내 파티가 풀(4명)이면 거부 → 대상이 이미 파티면 거부 → `g_pending_invites` 등록 → 대상에게 `S2C_PartyInvited` 전송 + 시전자에게 `SendSystemMessage` 피드백
- `C2S_PARTY_ACCEPT` 핸들러: pending invite 조회 → 초대자에게 파티 없으면 신규 생성(초대자 리더) → 양쪽 `party_id` 설정 → 기존/신규 멤버 전원에게 `S2C_PartyUpdate(joined)` 브로드캐스트
- `C2S_PARTY_REJECT` 핸들러: pending invite 제거 + 양쪽 시스템 메시지
- `C2S_PARTY_LEAVE` 핸들러 → `PlayerLeaveParty(session)` 공통 경로
- `GiveExpToKillerAndParty(killer, exp_gain)`: 파티 없으면 단독 `LevelUpPlayer`. 파티면 **온라인 파티원 균등 분배** (`share = max(1, exp_gain / online_members.size())`). 전투/스킬 처치 3개 지점(`C2S_ATTACK`, AoE 스킬, Line 스킬)에서 기존 단독 EXP 부여를 이 함수로 교체
- `PlayerLeaveParty(session)`: pending invite 정리 → `party_id.exchange(-1)` → members에서 제거 → **2명 미만 남으면 자동 해산**(전원 `party_id=-1` + `S2C_PartyUpdate(disbanded)`), 그 외엔 `S2C_PartyUpdate(left)` 통지 + **리더가 나가면 첫 멤버로 위임**
- disconnect 경로: 이름 있는 클라 정리 시 `PlayerLeaveParty(disconnected)` 호출 (파티 상태 누수 방지)

**클라이언트 (`client.cpp`)**:
- 채팅 입력에서 명령 파싱: `/invite <name>` / `/accept` / `/reject` / `/leave` → 해당 패킷 송신, 그 외는 일반 채팅
- `g_party_members` (자신 제외 최대 3명) + `g_party_member_ids` (미니맵/HP바 빠른 조회용 set)
- `S2C_PARTY_INVITED` → `"[Party] {이름} invited you. /accept or /reject"` 채팅 알림
- `S2C_PARTY_UPDATE` → joined/left/disbanded별 멤버 목록 갱신 + 채팅 로그
- **미니맵**: 파티원을 청록색 `(80,210,255)` 점으로 구분 표시 (일반 플레이어/NPC/보스와 분기)
- **파티 HP 바 HUD**: 채팅 패널 아래에 파티원별 이름 + HP 바 렌더링

**표절 회피**:
- 파티 자료구조/EXP 분배/리더 위임 모두 직접 작성. `Docs/npc.cpp`에 파티 개념 자체가 없음
- `unordered_map + mutex` 표준 라이브러리 조합

**검증**:
- Release x64 서버/클라/STRESS_TEST 모두 빌드 성공 (`S2C_PARTY_*` no-op 케이스 STRESS_TEST에 추가)
- 수동 검증: 2클라로 `/invite`→`/accept` 시 파티 결성, 미니맵 청록 표시 + HP 바 노출, NPC 처치 EXP 균등 분배, `/leave`·disconnect 시 해산/위임 정상

---

## 4. 검증 전략

| 도구 | 사용처 | 현재 상태 |
|---|---|---|
| `STRESS_TEST` (수정 완료) | CCU 한계 측정, delay 기반 자동 throttle | ✅ 빌드/실행 정상 |
| `CLIENT/client_sample` (SFML) | 시각적 회귀 확인 | ✅ 빌드 정상, Stage 2 view_list 사람 눈 확인 가능 |
| `--test-timer` 모드 | TimerManager 단위 검증 | ✅ 1000/1000, 16ms max lag |
| Performance Counter 로그 | PPS, 활성 NPC 수, 평균 패킷 RTT | Stage 4 이후 추가 예정 |
| ASAN/디버거 | 동시성 버그 스캔 | 필요 시 수동 |

**클라이언트 운영 전략** (오늘 추가 결정):
- **시각 확인**: `client_sample.exe` (사람이 직접 플레이)
- **부하 측정**: `STRESS_TEST.exe` (동접/지연만)
- **자동 통합 테스트**: 향후 `--test-client N` 모드 추가 검토 (게임 로직 정확성 자동 검증)

**알려진 테스트 주의 사항**: stress test 더미 클라는 2000x2000 무작위 좌표에 스폰되므로 시각 클라이언트 시야 안(15타일)에 우연히 들어올 확률은 매우 낮음(~5%). 시야에서 더미를 보려면 (a) 서버 spawn 좌표 좁히기, (b) `C2S_TELEPORT` 핸들러 구현 등이 필요. 현재는 정상 동작.

---

## 5. 파일 구조 (현재 상태)

### 신규 작성 완료
```
MMOSERVER_Termproject/MMOSERVER_Termproject/
├── Core/
│   ├── ObjectPool.h            ✅ Stage 2 Phase A
│   ├── Entity.h                ✅ Stage 2 Phase B
│   ├── NPC.h                   ✅ Stage 2 Phase D → Stage 4 확장
│   ├── OverlappedTypes.h       ✅ Stage 3
│   ├── TimerManager.h          ✅ Stage 3
│   ├── TimerManager.cpp        ✅ Stage 3
│   ├── World.h                 ✅ Stage 4 (NPC 스토리지)
│   ├── World.cpp               ✅ Stage 4
│   ├── NpcSpawner.h            ✅ Stage 4
│   ├── NpcSpawner.cpp          ✅ Stage 4
│   ├── LuaVM.h                 ✅ Stage 4.5 → 4.6 (NpcTick API 추가)
│   ├── LuaVM.cpp               ✅ Stage 4.5 → 4.6
│   └── GameConfig.h            ✅ Stage 4.6 (PDF 스펙 상수)
├── MMOSERVER_Termproject.cpp   ✅ Stage 4.6까지 통합 (NpcOnMove → Lua)
├── MMOSERVER_Termproject.vcxproj  ✅ NuGet(lua 5.5, tbb) + /utf-8 + LuaType=static
├── packages.config             ✅ lua 5.5.0 추가
└── protocol_2026.h             (그대로, Stage 5에서 패킷 추가 예정)

data/
├── npc_spawn.txt               ✅ Stage 4
└── npc_ai.lua                  ✅ Stage 4.5 (스켈레톤)
```

### Stage 5+ 에서 작성 예정
```
MMOSERVER_Termproject/MMOSERVER_Termproject/
├── Game/
│   ├── Combat.cpp              (Stage 5)
│   ├── AStar.h / .cpp          (Stage 6)
│   └── Map.h / .cpp            (Stage 6)
├── Db/
│   └── DbWorker.h / .cpp       (Stage 6)
└── data/
    └── obstacles.bin           (Stage 6)
```

### 수정한 외부 파일
- `STRESS_TEST/STRESS_TEST/NetworkModule.cpp` — 헤더 경로, 패킷명, 이동 패킷 포맷
- `client_sample` — 변경 없음 (이미 신 프로토콜에 맞음)

### 절대 수정 금지
- `RingBuffer` 클래스 — 표절 회피 핵심
- `Docs/npc.cpp` — 참고만, 절대 import 금지

---

## 6. 핵심 리스크 & 대응 (업데이트)

| 리스크 | 대응 | 현재 상태 |
|---|---|---|
| `Docs/npc.cpp` 구조 모방 → 표절 감점 20% | TimerManager는 cppreference idiom으로 자체 작성, 변수명/메서드명 의도적으로 다르게 | ✅ Stage 3 검토 완료 |
| 200K NPC 메모리 | NPC는 stable index 풀로 관리 (`shared_ptr` 대신 raw 배열) | Stage 4에서 적용 |
| TBB 사용으로 채점자 의심 | TBB 자료구조는 표절 감점 대상 아님. IOCP/Timer/DB만 직접 작성 | ✅ concurrent_hash_map 채택 결정 |
| DB 일정 지연 | Stage 5 시점에 DB stub 인터페이스 추상화 | Stage 5 시 적용 |
| Send 패킷 비배치 → syscall 폭증 | 계획에는 없으나 성능 한계 시 도입 검토 | Stage 4 측정 후 결정 |
| `std::cout` 직렬화가 핫패스에 있어 부하 시 병목 | 향후 비동기 로깅 큐로 분리 검토 | Stage 4 측정 후 결정 |

---

## 우선순위 TOP 3 (남은 작업 기준, 2026-05-30 업데이트)

**현재 상태**: PDF 명시 항목(IOCP/타이머/스크립트/시야/AI/A\*/DB) 전부 충족 + 스킬·보스·시각 완성도 강화 + **파티(10점) + 아이템(20점) + 퀘스트(25점) 완료**. 가산점 풀세트 확보. 남은 작업은 (선택) SQL Server 백엔드 / 부하 측정 강화.

### ✅ 완료: 파티 시스템 (10점) — Stage 7.4
- 패킷 6종, 온라인 파티원 EXP 균등 분배, 미니맵 청록 표시 + HP 바 HUD. 상세는 Stage 7.4 섹션 참조.

### ✅ 완료: 아이템 시스템 (20점) — Stage 8
- 카탈로그 8종(data/items.txt), NPC 드롭 + 수동 줍기(G), I 인벤 패널 + 숫자키, 무기 공격력/방어구 max_hp, DB 영속성. 상세는 Stage 8 섹션 참조.

### ✅ 완료: 퀘스트 시스템 (25점) — Stage 9
- 슬레이 + 대화 + 연쇄를 한 데이터 주도(`data/quests.txt`) 아키텍처로 구현. 장로 대화 UI(T/J/Y), 처치 카운트 훅 3곳, DB 영속성. 상세는 Stage 9 섹션 참조.

### 1순위: (선택) SQL Server 백엔드 / STRESS_TEST 강화
- JSON stub → OdbcBackend 교체 (IDbBackend 그대로). 더미에 공격/줍기 추가 시 아이템 부하도 측정 가능

### (선택) SQL Server 백엔드
- 현재는 JSON 파일 stub. 채점 환경에 SQL Server 있으면 OdbcBackend 추가만 하면 교체 (IDbBackend 인터페이스 그대로)

### (보조) STRESS_TEST 강화
- 현재 더미는 이동/로그인만. 채팅/공격/스킬 발화 추가하면 Stage 5+/7.1 시스템 부하 측정 가능

---

## 부록 A: 측정 결과 기록

### Stage 1 완료 시점 (Release x64, 30초 스트레스)
- Connect 589, Login 589, Disconnect 469, Reject 0, Error 0, stderr 0byte
- 피크 동접 ~120

### Stage 2 완료 시점 (Release x64, 30초 스트레스)
- Connect 589, Login 589, Disconnect 485, Net 104, Error 0
- view_list 차분 추가로 인한 약간의 오버헤드 (NPC 없는 환경에서)

### Stage 2 + concurrent_hash_map (Release x64, 30초, 3회 평균)
- Net 72 (개별 49/48/118로 노이즈 큼)
- 단일 30초 실행으로는 명확한 우열 판정 어려움 (stress test 자체의 측정 한계)

### Stage 3 TimerManager 단위 테스트
- 1000개 이벤트, 50~5000ms 랜덤 딜레이
- Fired 1000/1000, Ordering PASS, 평균 lag 7ms, max lag 16ms, 100ms 초과 0건

### Stage 3 완료 시점 회귀 (Release x64, 30초)
- Connect 589, Login 589, Disconnect 494, Net 95, Error 0
- TimerManager idle 동작 (스케줄된 이벤트 없음) → 회귀 영향 없음

### Stage 4 완료 시점 (Release x64, 30초, NPC 200K 활성)
- (Roaming-only 1차 검증) Connect 1,092, Login 1,092, Disconnect 962, Error 0, Reject 0
- (Agro 추적 추가 후 2차 검증) Connect 557, Login 557, Disconnect 525, Error 0, Reject 0
  - Agro NPC가 시야에 들어온 플레이어를 끊임없이 추적 → S2C_MoveObject 패킷 폭증으로 처리량 감소
  - 안정성은 유지 (0 error). Stage 5에서 send 배치 검토 필요
- 200K NPC 초기화 시간 < 0.5초 (단발 할당)
- NpcSpawner: 결정적 시드(0xA37E1A20)로 매번 동일한 NPC 배치

### Stage 5 완료 시점 (Release x64, 30초, NPC 200K 활성, 2026-05-22)
- Connect **588**, Login **588**, Disconnect 520, **Error 0**, **Reject 0**, stderr 0 byte
- Stage 5 신규 코드 (전투/HP회복/EXP/사망부활/채팅, 패킷 5종 추가) 적용 상태에서 Stage 4 2차(557)와 동등 이상
- signed char 패킷 크기 버그 수정으로 클라 측 209B 패킷도 정상 처리
- STRESS_TEST는 GUI 더미라 공격/채팅을 발화하지 않음 — 전투 코드 자체의 부하는 별도 멀티 클라 시각 검증으로 보완

---

## 부록 B: 핵심 구현 파일 절대 경로

- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\MMOSERVER_Termproject\MMOSERVER_Termproject\MMOSERVER_Termproject.cpp`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\MMOSERVER_Termproject\MMOSERVER_Termproject\protocol_2026.h`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\MMOSERVER_Termproject\MMOSERVER_Termproject\Core\` (전체)
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\STRESS_TEST\STRESS_TEST\NetworkModule.cpp`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\CLIENT\client_sample\client.cpp`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\Docs\npc.cpp` (참고용, 직접 import 금지)

---

## Stage 7.5 — 클라 전투 메시지창 + EXP 바 렌더링 픽스 (2026-05-28)

### 목표
- 채팅 패널을 게임 메시지창으로 확장: 전투 이벤트(공격/피격/처치+경험치)를 한글로 로그 출력
- 한글 메시지가 정상적으로 렌더링되도록 폰트/인코딩 경로 보강
- 기존에 채워지지 않던 EXP 바 fill 렌더링 수정

### 구현 내용

**1) 전투 로그 3종 (`CLIENT/client_sample/client.cpp`)**
- `S2C_DAMAGE` 핸들러에 두 메시지 추가:
  - 내가 NPC를 때림 → `"용사가 {몬스터이름}를 때려서 {damage}의 데미지를 입혔습니다."`
  - NPC가 나를 때림 → `"{몬스터이름}의 공격으로 {damage}의 데미지를 입었습니다."`
  - 마지막 일격(`new_hp <= 0`)이면 NPC 이름을 `g_last_killed_npc_name`에 저장
- `S2C_STATUS_CHANGE` 핸들러: 내 exp가 증가하고 직전 처치 이름이 남아 있으면 `"{이름}를 무찔러서 {gained}의 경험치를 얻었습니다."` 출력

**2) 한글 렌더링 경로**
- 폰트 로딩 순서: `C:/Windows/Fonts/malgun.ttf` → `malgun.ttf` → `cour.ttf` (fallback)
- 채팅 로그 그리기에서 `sf::String::fromUtf8(...)`로 변환 — 기존 std::string 직패스는 Latin-1로 잘못 해석되던 문제 해결
- `client_sample.vcxproj`의 4개 Configuration 모두에 `/utf-8` 컴파일 옵션 추가 — Korean Windows에서 UTF-8 소스를 CP949로 잘못 해석하던 문제 해결

**3) EXP 바 fill 렌더링 픽스**
- 기존: `sf::Sprite` + `setRepeated(true)` + 큰 `IntRect`로 16×32 fill 타일을 가로 반복 — 일부 GPU/드라이버에서 미렌더링
- 변경: `sf::RectangleShape` 두 개로 단순화 (본체: 황금색 240,200,60 / 상단 하이라이트: 밝은 금색 255,235,140 α200)
- 데이터(`g_my_exp`) 파싱과 LERP 보간은 그대로, fill의 시각 표현만 교체

### 변경 파일
- `CLIENT/client_sample/client.cpp` — 메시지 출력 로직(2곳), 폰트 fallback 로딩, UTF-8 채팅 렌더링, EXP 바 fill 재구현
- `CLIENT/client_sample/client_sample.vcxproj` — 4 configuration에 `/utf-8` 옵션

### 검증
- 빨간 디버그 박스로 RectangleShape 렌더링 경로 확인 → 정상
- 콘솔 디버그 로그로 `exp_ratio` / `g_my_exp` / `g_my_level` 값 확인 → 정상 갱신
- 모든 디버그 코드는 사용자 확인 후 제거 완료

---

## ✅ Stage 8 — 아이템 시스템 (가산점 20점, 완료, 2026-05-29)

가산점 4요소 **소모 + 누적(스택) + 인벤토리 + 장착**을 모두 구현. 기존 DB/전투/브로드캐스트/HUD 패턴 위에 얹음.

### 설계 결정 (사용자 합의)
- **줍기**: 수동 키 `G` (근처 바닥 아이템 줍기)
- **카탈로그**: 포션 2종 + 무기 3티어 + 방어구 3티어 (총 8종)
- **인벤 UI**: `I` 토글 패널 + 숫자키(패널 열렸을 때만 슬롯 조작 — 모달)
- **블라스트 최소화**: 무기 공격력은 `atk_bonus`(atomic, lockless) 분리 / 방어구 보너스는 `max_hp`에 **직접 합산**(기존 max_hp 사용처 무수정), DB는 base(=max_hp−방어구보너스)로 저장해 재장착 중복 합산 방지

### 신규 파일
- `data/items.txt` — 카탈로그 (`id name type value stack_max drop_weight`, sscanf 라인 파서). type 0=소모/1=무기/2=방어구
- `Core/Item.h / .cpp` — `ItemDef`/`ItemType`, `g_item_defs`, `GetItemDef`, `LoadItemDefs`(경로 후보 탐색), 드롭 로직(`RollShouldDrop` 일반35%/Agro50%/Boss100%, `RollDropItems` drop_weight 가중 랜덤, 보스는 무기+방어구+포션 보장)

### 프로토콜 (`protocol_2026.h`) — enum 끝에 append (기존 ID 불변)
- `C2S_PICKUP` / `C2S_USE_ITEM(slot)` / `C2S_EQUIP_ITEM(slot)` / `C2S_UNEQUIP_ITEM(which)`
- `S2C_ITEM_DROP(drop_id,item_id,x,y)` / `S2C_ITEM_REMOVE(drop_id)` / `S2C_INVENTORY`(슬롯 배열 + 장착 ID, 전체 스냅샷)
- `MAX_INVENTORY_SLOTS=20` (패킷 174B < 256 보장), `InvSlotNet{item_id, qty}`

### 서버 (`MMOSERVER_Termproject.cpp`, `Core/GameConfig.h`, `Core/TimerManager.h`)
- `Player`: `inv_lock` + `inventory`(vector<pair>) + `equipped_weapon_id/armor_id`(atomic) + `atk_bonus`(atomic)
- 바닥 아이템: `g_ground_items`(map) + `g_ground_mutex` + `g_next_drop_id`(DROP_ID_START=2,000,000), `BroadcastToSectorPlayers`(3×3 섹터)
- `SpawnNpcLoot()` — NPC 사망 3곳(기본공격/AoE/Line)에서 호출, 드롭 생성 + `GroundItemExpire` 60초 타이머
- 핸들러: `C2S_PICKUP`(claim-first 이중줍기 방지, 가득이면 바닥 복귀) / `C2S_USE_ITEM`(소모품 HP회복) / `C2S_EQUIP_ITEM`(swap, 슬롯 −1/+1로 오버플로 불가) / `C2S_UNEQUIP_ITEM`
- 헬퍼: `AddToInventoryLocked`(스택/신슬롯), `SendInventory`, `SendStatusChange`(본인+파티)
- 데미지 계산 3곳에 `+ atk_bonus` 가산
- `GameConfig`: `MAX_INVENTORY_SLOTS`(protocol), `GROUND_ITEM_EXPIRE_MS=60000`, `ITEM_PICKUP_RANGE=1`, `DROP_ID_START`
- `TimerManager`: `TimerEventKind::GroundItemExpire` 추가

### DB 영속성 (`Core/Db/DbTypes.h`, `JsonFileBackend.cpp`)
- `PlayerSnapshot`에 `inventory[] / equipped_weapon_id / equipped_armor_id` 추가
- Save: `"inventory":[[id,qty],...],"weapon":W,"armor":A` 직렬화. Load: `ReadIntPairArray` 미니 파서(키 없으면 빈 인벤 — 구버전 호환)
- `SnapshotPlayer`: max_hp를 base로 보정 저장 / `OnPlayerSpawn`: 복원 + 보너스 재계산 + AvatarInfo 직후 `SendInventory`

### 클라이언트 (`CLIENT/client_sample/client.cpp`)
- 아이템 메타 테이블(items.txt와 동일 ID), `g_ground_items` / `g_inventory` / `g_equipped_*` / `g_inv_open`
- 핸들러 `S2C_ITEM_DROP/REMOVE/INVENTORY`, 바닥 아이템(회전 보석) 렌더, `I` 토글 인벤 패널(5×4 그리드 + 장착 슬롯 + 힌트)
- 입력: `I` 토글, `G` 줍기, **패널 열렸을 때** 숫자키로 사용/장착(클라가 타입 판별) + `R/F` 무기/방어구 해제, 패널 닫히면 숫자키는 기존 텔레포트 유지(모달 분기)

### STRESS_TEST
- `S2C_ITEM_DROP/REMOVE/INVENTORY` no-op 케이스 추가

### 검증 (Release x64, 2026-05-29)
- 서버/클라/STRESS_TEST 3종 모두 빌드 0 에러 (서버 0경고, stress는 기존 C4819 한글 인코딩 경고만)
- 부팅: `[Item] Loaded 8 item defs.` / 200K NPC + Lua + DB 정상 / 무크래시
- 30초 stress(NPC 200K 활성): 서버·stress 생존, **신규 stderr 0건**, **588개 player JSON이 신규 포맷(`"inventory":[],"weapon":-1,"armor":-1`)으로 저장** → SnapshotPlayer/Save 직렬화 + 매 로그인 SendInventory가 churn 하에서 안정. 처리량 Stage 5/6 베이스라인(586~588)과 동등
- 수동 검증(권장): NPC 처치 → 바닥 보석 → `G` 줍기 → `I` 패널 → 숫자키로 포션(HP↑)·무기(데미지↑)·방어구(maxHP↑) → `R/F` 해제 → 재로그인 시 인벤/장착 유지

### 표절 회피
- 카탈로그/드롭/인벤/장착 로직 모두 자체 작성. JSON 배열 직렬화도 외부 라이브러리 없이 수동. `Docs/npc.cpp`에 아이템 개념 없음

---

## ✅ Stage 9 — 퀘스트 시스템 (가산점 25점, 완료, 2026-05-30)

가산점 최대 항목인 퀘스트를 **슬레이(처치) + 대화(수락/보상) + 연쇄(선행 퀘스트)** 풀세트로 구현. 기존 아이템/DB/브로드캐스트/HUD 패턴 위에 얹음.

### 설계 결정
- **단일 진실원 분리**: 서버가 퀘스트 로직(목표/보상/선행/상태)의 단일 진실원, 클라는 텍스트(제목/설명) 메타만 미러링. 패킷은 id/카운트/상태만 → 경량. (아이템 시스템의 `ClientItemMeta` 미러 패턴과 동일)
- **마을 NPC는 클라 시각 마커**(`g_village_npcs[]`)일 뿐 서버 엔티티가 아님 → 상호작용은 "근처에서 키 입력 → 패킷 + 서버 좌표 근접 검증" 방식으로 우회 (장로 = index 0).
- **슬레이 타겟 매칭**: NPC 이름(`Slime_00042`)의 `_` 앞 접두사 비교. `SlimeKing`이 `Slime`에 오매칭되지 않도록 접두사 직후가 `_` 또는 끝일 때만 일치.
- **킬 크레딧**: 파티가 있으면 온라인 파티원 전원, 없으면 처치자 단독 (EXP 분배와 동일하게 거리 무관, 온라인 멤버 기준).

### 신규 파일
- `data/quests.txt` — 카탈로그 (`id prereq giver target_species count reward_exp reward_item reward_qty`, sscanf 라인 파서). 장로 3단 연쇄: 슬라임(3)→쥐(5)→고블린(5).
- `Core/Quest.h / .cpp` — `QuestDef`, `g_quest_defs`, `GetQuestDef`, `LoadQuestDefs`(경로 후보 탐색), `QuestSpeciesMatches`(접두사 매칭). `Item.h/.cpp` 구조 모방.

### 프로토콜 (`protocol_2026.h`) — enum 끝에 append (기존 ID 불변)
- `C2S_QUEST_INTERACT(npc_index)` / `C2S_QUEST_ACTION(quest_id, action: 0=accept,1=turnin)`
- `S2C_QUEST_DIALOGUE(npc_index, quest_id, kind)` — kind: 0=Offer / 1=InProgress / 2=ReadyTurnIn / 3=None
- `S2C_QUEST_UPDATE(quest_id, kill_count, target_count, state)` — state: 0=active / 1=completed

### 서버 (`MMOSERVER_Termproject.cpp`, `Core/GameConfig.h`)
- `Player`: `quest_lock` + `vector<QuestProgress>{quest_id, kill_count, state}`
- `GameConfig`: `QUEST_GIVER_X/Y=985`, `QUEST_INTERACT_RANGE=3` (chebyshev). client `g_village_npcs[0]`와 일치.
- main(): 아이템 로드 직후 `LoadQuestDefs` (경로 후보 3개).
- 헬퍼: `SendQuestUpdate`(1건), `SendAllQuestUpdates`(로그인 복원 동기화), `OnNpcKilledForQuest`(처치 카운트).
- `C2S_QUEST_INTERACT` 핸들러: 근접 검증 → 대화 종류 판정(우선순위 ReadyTurnIn > InProgress > Offer > None) → `S2C_QuestDialogue`.
- `C2S_QUEST_ACTION` 핸들러: 근접 재검증 → accept(선행 완료+미보유 검증 후 추가) / turnin(카운트 충족 시 state=completed + 보상: EXP는 `LevelUpPlayer`, 아이템은 `AddToInventoryLocked`+`SendInventory`).
- **처치 훅 3지점**: 기본공격/AoE스킬/Line스킬 사망 처리에서 `GiveExpToKillerAndParty` 직후 `OnNpcKilledForQuest(session, n)` 호출.
- `OnPlayerSpawn`: 인벤 복원 직후 `SendAllQuestUpdates`로 퀘스트 로그 영속 동기화.

### DB 영속성 (`Core/Db/DbTypes.h`, `JsonFileBackend.cpp`)
- `PlayerSnapshot`에 `std::vector<std::array<int,3>> quests` 추가.
- Save: `"quests":[[id,cnt,state],...]` 직렬화. Load: `ReadIntTripleArray` 미니 파서(키 없으면 빈 목록 — 구버전 호환).
- `SnapshotPlayer`: `quest_lock` 잡고 저장 / `OnPlayerSpawn`: 복원(def 없는 퀘스트는 스킵).

### 클라이언트 (`CLIENT/client_sample/client.cpp`)
- `ClientQuestMeta` 텍스트 테이블(quests.txt와 동일 id) + `g_quests` 상태 + 대화창 상태.
- 입력: **T**=장로 대화(±3타일 근접 시 `C2S_QuestInteract`), **J**=퀘스트 로그 토글, 대화창 모달에서 **Y**=수락/보상수령·**Esc**=닫기.
- 핸들러 `S2C_QUEST_DIALOGUE`(대화창 open) / `S2C_QUEST_UPDATE`(g_quests upsert + 채팅 로그).
- 렌더: 하단 중앙 대화창(kind별 분기) + 우측 퀘스트 로그 패널 + 장로 마커 "!"→"?"(보고 가능 시 금색). 인벤 패널과 동일한 `RectangleShape`+`fromUtf8` 스타일.

### STRESS_TEST
- `S2C_QUEST_DIALOGUE/UPDATE` no-op case 추가 (기존 PARTY/ITEM no-op 패턴).

### 검증 (Release x64, 2026-05-30)
- 서버 / `client_sample` / `STRESS_TEST` 3종 모두 빌드 0 에러 (stress는 기존 C4819 한글 인코딩 경고만).
- 부팅: `[Quest] Loaded 3 quest defs.` / 200K NPC + Lua + DB 정상 / **stderr 0건**.
- 수동 검증(권장): 장로(985,985) `T` → 대화창 q1 Offer → `Y` 수락 → `J` 로그 0/3 → SW 초원 Slime 처치 1/3→3/3 → 장로 `T` ReadyTurnIn → `Y` 보상(EXP/포션, `I`로 확인) → q2 연쇄 등장 → 다른 종족(Rat)은 q1 미카운트 → 재로그인 시 진행/완료 유지(`data/players/<n>.json`에 `"quests":[...]`).

### 표절 회피
- 카탈로그/상태머신/연쇄/보상 로직 모두 자체 작성. JSON 3쌍 배열 직렬화도 외부 라이브러리 없이 수동. `Docs/npc.cpp`에 퀘스트 개념 없음.
