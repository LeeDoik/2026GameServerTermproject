# Aetheria Online MMO 서버 재설계 계획

> 최종 업데이트: 2026-05-21
> Stage 1, 2, 3 완료. Stage 4부터 진행 예정.

---

## 0. 진행 현황 (2026-05-21)

| 단계 | 상태 | 핵심 결과 |
|---|---|---|
| Stage 1 — 안정성 핫픽스 | ✅ 완료 | 30초 스트레스 0 에러/0 크래시 |
| Stage 2 — Entity + view_list + ObjectPool | ✅ 완료 | 시야 경계 Remove 누락 버그 해결 |
| Stage 2 (추가) — concurrent_hash_map 교체 | ✅ 완료 | shared_mutex 글로벌 락 제거 |
| Stage 3 — TimerManager (priority_queue + cv) | ✅ 완료 | 1000/1000 이벤트, max lag 16ms |
| Stage 4 — NPC 스폰 + AI (FSM + Roaming + Agro) | ⏳ 다음 | TimerManager 대기 중 |
| Stage 5 — 전투 + HP/EXP + 사망/리스폰 + 채팅 | ⏳ 보류 | |
| Stage 6 — A* + 장애물 + DB | ⏳ 보류 | |
| Stage 7 — 가산점 (스킬/보스/파티/아이템/퀘스트) | ⏳ 보류 | |

**현재 부하 테스트 한계**: Release x64 기준 동접 ~100~200에서 stress test의 delay-throttle이 발동. 진짜 5000 CCU는 NPC 200K 도입(Stage 4) + send 배치(미계획) 이후에야 측정 의미.

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

### ⏳ Stage 4 — NPC 200K 스폰 + AI (FSM + Roaming + Agro) — *다음 작업*

**작업**:
- `Core/NPC.cpp` 작성 — Stage 2에서 만든 NPC 헤더의 메서드 구현
- `Core/World.h/.cpp` 작성 — players와 npcs 통합 컨테이너, AddEntity/RemoveEntity/MoveEntity/GetNeighbors
- `Core/NpcSpawner.cpp` — JSON 스크립트 로딩 (`data/npc_spawn.json`), 추가 요소 5점 확보
- `NPC::OnTimer(MOVE)` — Peace+Roaming은 스폰 중심 20x20 내 랜덤 1칸 이동, Peace+Fixed는 미동
- `NPC::OnTimer(MOVE)` — Agro 상태 진입 시 타깃 추적. 11x11 감지는 시야 갱신 시점에 진입한 플레이어 확인
- **Lazy AI 활성화**: 시야에 플레이어가 없으면 NPC 타이머를 큐에 넣지 않음. 플레이어가 시야에 진입할 때만 wake_up

**왜 이 순서**: Timer가 먼저 있어야 NPC AI 동작 가능 → ✅ Stage 3 완료. View_list가 먼저 있어야 Agro 감지가 효율적 → ✅ Stage 2 완료. 이제 NPC 도입 가능.

**검증**: 스트레스 테스트로 1,000 CCU + 20만 NPC 활성. CPU/메모리 측정. 시야 처리 효율 확인.

---

### ⏳ Stage 5 — 전투 + HP/EXP + 사망/리스폰 + 채팅 — *과제 기본 스펙 마무리*

**프로토콜 추가** (`protocol_2026.h`):
- `S2C_ATTACK_ANIM`, `S2C_DAMAGE`, `S2C_DEATH`, `S2C_RESPAWN`, `S2C_LEVEL_UP`
- (C2S_ATTACK은 이미 정의됨)

**작업**:
- `Server/Game/Combat.cpp` — A키 = 인접 4타일 NPC 대상 동시 데미지. 데미지 공식은 기획서 6.2 적용
- HP 회복 5초 타이머, 사망 시 EXP 50% 감소(과제 명세) + 시작 위치로 텔레포트
- NPC 사망 → 30초 리스폰 타이머 등록
- `C2S_CHAT` → `S2C_CHAT_MESSAGE` 시야 내 브로드캐스트

**검증**: 클라이언트로 NPC 잡고 EXP 오르고 레벨업 → 사망 → 리스폰 시나리오.

---

### ⏳ Stage 6 — A* 길찾기 + 장애물 + DB 영속성

**작업**:
- `Server/Game/AStar.cpp` — 격자 A* (octile distance). NPC Agro 추적 시에만 호출. 경로 캐싱
- `Server/Game/Map.cpp` — 장애물 비트맵 로딩 (2000x2000 비트 = 500KB). 이동/스폰 시 충돌 검사
- `Server/Db/DbWorker.cpp` — ODBC 연결 풀, 별도 워커 스레드가 큐 폴링. 비동기 완료는 `PostQueuedCompletionStatus`로 워커에 통보 (`IO_DB_DONE`)

**리스크 대응**: Stage 5 끝나는 시점에 DB stub(JSON 파일 dump)으로 우회 가능한 인터페이스 추상화 → DB 일정 밀려도 영속성 흉내는 가능

---

### ⏳ Stage 7 — 가산점 추가 요소 — *게임성 40점이 최종 점수 가른다*

| 우선순위 | 항목 | 점수 | 권장 이유 |
|---|---|---|---|
| A | 스크립트 NPC 배치 | 5점 | Stage 4에서 사실상 완료 |
| B | 스킬: 범위/방향성 + 버프 | 10점 | AoE는 시야 내 ID 순회로 자연 구현 |
| C | 보스 패턴 (이동/채팅/스킬) | 5-10점 | NPC 타입 추가만으로 가능 |
| D | 파티 시스템 | 10점 | 파티 → 시야 동기화 → EXP 분배 |
| E | 아이템: 소모 + 누적 + 인벤 + 장착 | 20점 | DB 스키마 확장 필요 |
| F | 퀘스트 풀세트 | 25점 | 슬레이/대화/연쇄 — 시간 많이 필요 |

**전략**: A→B→C→D 순으로 안전 30점 확보 후 시간 남으면 E/F.

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
│   ├── NPC.h                   ✅ Stage 2 Phase D (스켈레톤)
│   ├── OverlappedTypes.h       ✅ Stage 3
│   ├── TimerManager.h          ✅ Stage 3
│   └── TimerManager.cpp        ✅ Stage 3
├── MMOSERVER_Termproject.cpp   ✅ 대폭 수정 (1, 2, 3단계 통합)
├── MMOSERVER_Termproject.vcxproj  ✅ 신규 파일 등록 + /utf-8 플래그
└── protocol_2026.h             (그대로, Stage 5에서 패킷 추가 예정)
```

### Stage 4에서 작성 예정
```
MMOSERVER_Termproject/MMOSERVER_Termproject/
├── Core/
│   ├── NPC.cpp                 (Stage 4)
│   ├── World.h / .cpp          (Stage 4)
├── Game/
│   ├── NpcSpawner.cpp          (Stage 4)
│   ├── Combat.cpp              (Stage 5)
│   ├── AStar.h / .cpp          (Stage 6)
│   └── Map.h / .cpp            (Stage 6)
├── Db/
│   └── DbWorker.h / .cpp       (Stage 6)
└── data/
    ├── npc_spawn.json          (Stage 4)
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

## 우선순위 TOP 3 (남은 작업 기준)

### 1순위: **Stage 4 NPC + AI (2~3주차)**
- 과제 기본 스펙의 절반(NPC 20만, AI, 로밍, Agro, 30초 리스폰)이 여기서 결정
- 표절 감점 핵심 영역이므로 코드 작성 단계에서 가장 신중
- TimerManager가 처음으로 본격 사용됨 → 실전 부하 측정 가능

### 2순위: **Stage 5 전투/HP/EXP/채팅 + Stage 6 DB (4~5주차)**
- 평가표 "구현 50%"의 명시 항목 모두 포함
- DB는 누락 불가. Stage 5 시점에 stub 인터페이스 먼저 만들고 Stage 6에서 실제 ODBC 연결

### 3순위: **Stage 7 가산점 (5~6주차)**
- 게임성 40점 효율 순으로 (스크립트 → 스킬 → 보스 → 파티)
- 시간 남으면 아이템·퀘스트 (점수 크지만 작업량도 큼)

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

---

## 부록 B: 핵심 구현 파일 절대 경로

- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\MMOSERVER_Termproject\MMOSERVER_Termproject\MMOSERVER_Termproject.cpp`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\MMOSERVER_Termproject\MMOSERVER_Termproject\protocol_2026.h`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\MMOSERVER_Termproject\MMOSERVER_Termproject\Core\` (전체)
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\STRESS_TEST\STRESS_TEST\NetworkModule.cpp`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\CLIENT\client_sample\client.cpp`
- `C:\한국공학대학교\4학년 1학기\게임서버 프로그래밍\텀프로젝트\2026GameServerTermproject\Docs\npc.cpp` (참고용, 직접 import 금지)
