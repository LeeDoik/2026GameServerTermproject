# Aetheria Online — C++ IOCP MMORPG 서버

게임 서버 프로그래밍 텀 프로젝트 (2026.04 – 06, 1인). 서버의 네트워크 · 타이머 · DB · 스크립트 계층을 직접 구현하고,
부하 테스트로 병목을 찾아 고친 기록입니다.

| | |
|---|---|
| **규모** | 2000×2000 월드 · NPC 20만 · 동시접속 상한 1만(`MAX_PLAYERS`) |
| **측정 결과** | 동시접속 10,000 · 이동 왕복 지연 약 20ms (개인 PC 1대에서 서버와 부하 클라이언트를 함께 실행) |
| **스택** | C++20 · Win32 IOCP · Winsock · Intel TBB · Lua 5 · MS SQL Server(ODBC) · SFML(클라이언트) |

> 측정값은 개인 테스트 환경 수치이며 상용 운영 실적이 아닙니다. 1만은 과제가 정한 서버 상한이고,
> 이때 CPU에는 여유가 있었습니다. 서버와 부하 클라이언트를 분리한 측정은 하지 못했습니다.

---

## 아키텍처

```
          ┌──────────── 하나의 IOCP ────────────┐
 클라 ──▶ │ IO_RECV / IO_SEND / IO_ACCEPT        │
 타이머 ─▶ │ IO_TIMER   (8샤드 min-heap 디스패처)  │ ──▶ 워커 스레드 (코어 수만큼)
 DB ────▶ │ IO_DB_DONE (DB 전용 스레드가 완료 통지) │      └ thread_local Lua VM
          └──────────────────────────────────────┘
```

- **네트워크 I/O · 타이머 만기 · DB 완료를 하나의 IOCP로 합쳤습니다.** 모든 게임 상태 변경이 워커 컨텍스트에서만 일어나 동기화 규칙이 하나로 정리됩니다. `OVERLAPPED_EX` · `TimerOverlapped` · `DbOverlapped`는 앞 두 필드(`WSAOVERLAPPED`, `IO_TYPE`)를 같은 레이아웃으로 맞춰 `IO_TYPE`만 보고 분기합니다.
- **패킷 조립**: 가변 길이(Length-Prefix) 패킷을 세션별 `RingBuffer`(8KB)로 조립합니다.
- **DB는 게임 루프를 막지 않습니다.** 저장 · 로드 요청은 Job Queue로 DB 전용 스레드에 넘기고, 완료는 `PostQueuedCompletionStatus(IO_DB_DONE)`으로 워커에 돌아옵니다. 블로킹 ODBC 호출은 그 스레드 안에만 있습니다.
- **DB 백엔드 교체 가능**: `IDbBackend` 인터페이스 아래 SQL Server(ODBC)와 JSON 파일 백엔드가 있고, SQL Server 연결에 실패하면 JSON으로 자동 폴백합니다.

---

## NPC 20만을 버티는 방법

| 방법 | 내용 |
|---|---|
| **섹터 기반 AOI** | 월드를 20×20 섹터로 나누고 주변 9섹터만 시야 검사 |
| **Lazy AI** | NPC는 평소 비활성. 플레이어 시야에 들어올 때 CAS(`compare_exchange`)로 한 번만 깨우고, 시야가 비면 스스로 잠듦 → 활성 AI 수가 NPC 총량이 아니라 **동접 수**를 따름 |
| **워커별 Lua VM** | `thread_local` Lua VM + stateless `OnTick` — AI 핫패스에 락 없음 |
| **동시성 안전한 전투** | HP · EXP 갱신은 CAS로 처리해, HP를 0으로 만든 호출만 사망 처리를 1회 수행 |

---

## 병목을 찾아 고친 과정

### 1. 3,000 동접의 벽 — 원인은 NPC 연산이 아니라 락이었다

부하를 올리자 약 3,000 동접에서 응답이 느려졌습니다. 처음엔 NPC 연산량을 의심했지만,
프로파일러와 CPU 점유율로 재 보니 **모든 워커가 전역 `lua_State`를 보호하는 mutex 하나에 줄을 서고** 있었습니다.

| 병목 | 수정 |
|---|---|
| 전역 Lua mutex | Lua VM을 워커별 `thread_local`로 분리, AI 로직을 stateless로 변경 |
| 단일 타이머 스레드 · 단일 큐 | 8샤드 타이머(샤드별 min-heap + 디스패처) |
| 전역 송신 오브젝트 풀 | 8샤드 송신 풀 |
| 패킷마다 `WSASend` | 세션별 송신 코얼레싱 — 한 틱에 쌓인 패킷을 8KB 한도로 묶어 1회 전송 |
| Nagle 지연 | `TCP_NODELAY` |

같은 조건으로 다시 측정해 동접 10,000 · 왕복 약 20ms를 확인했습니다.
이 시점 프로파일러에서 self-time 상위는 `WSASend/recv`와 플레이어 맵 조회였고, Lua 호출은 핫스팟에 잡히지 않았습니다.
남은 한계는 CPU 연산이 아니라 네트워크 I/O 쪽입니다.

![CPU 프로파일](Docs/발표자료/CPU%20보고서.png)

### 2. 재접속하면 위치가 사라지는 버그 — `SQL_NO_DATA`

MySQL에서 SQL Server로 옮긴 뒤, 재접속할 때마다 마지막 위치가 아니라 마을 랜덤 위치에 스폰됐습니다.

- **추적**: 접속 → 로드 결과 `exists=false` → 저장 함수 → 트랜잭션 → ODBC 반환값 순으로 따라갔습니다.
- **원인**: 저장 시 `player_inventory` · `player_quests`를 지우고 다시 쓰는데, 신규 플레이어는 지울 행이 0개입니다. SQL Server ODBC는 0행 DELETE에 `SQL_NO_DATA`를 반환하고(ODBC 표준, MySQL 드라이버는 `SQL_SUCCESS`), 이를 실패로 판정해 **저장 트랜잭션 전체가 롤백**되고 있었습니다.
- **수정**: DELETE 검사에만 `SQL_NO_DATA`를 정상으로 인정하는 `OkDelete()`를 적용했습니다. 진단 출력이 리다이렉트 시 버퍼에 묻히던 문제도 `fflush(stderr)`로 고쳤습니다.
- **확인**: `players` 테이블에 로그아웃 좌표가 남고 ODBC 오류가 0건인 것을 확인했습니다. (커밋 `67dea03`)

---

## 게임 기능

4개 지역 · 스킬 3종(광역 · 직선 · 회복) · A* 경로 탐색 몬스터 AI(Peace / Agro, 로밍, 30초 부활) ·
아이템 드롭 · 장비 · 인벤토리 · 연쇄 퀘스트 · 파티(경험치 공유) · 2페이즈 보스 · 채팅 · 사망 시 EXP 50% 감소와 마을 부활 ·
위치 · 레벨 · 인벤토리 · 퀘스트 DB 영속화

NPC 배치 · 아이템 · 퀘스트 · 장애물은 `data/*.txt`, NPC AI는 `data/npc_ai.lua`로 분리했습니다.

---

## 빌드와 실행

**요구 사항**: Windows · Visual Studio 2022(v143) · NuGet 패키지 복원(Intel TBB, Lua, SFML)

| 프로젝트 | 솔루션 | 설명 |
|---|---|---|
| 서버 | `MMOSERVER_Termproject/MMOSERVER_Termproject.sln` | 포트 3500 |
| 클라이언트 | `CLIENT/client_sample.sln` | SFML 클라이언트 |
| 부하 테스트 | `STRESS_TEST/STRESS_TEST.sln` | 그래픽 부하 클라이언트 |
| 헤드리스 부하 테스트 | `STRESS_TEST/headless_main.cpp` | 실행 후 서버 IP를 입력 |

**DB 설정** (`data/db.ini`)

- `enabled=0`이면 SQL Server 없이 JSON 파일 백엔드(`data/players/`)로 바로 실행됩니다.
- SQL Server를 쓰려면 `data/schema_mssql.sql`로 DB와 로컬 테스트용 로그인을 만들고, `conn`의 드라이버 이름을 설치된 ODBC 드라이버에 맞춥니다. `db.ini`의 계정은 이 스크립트가 만드는 **로컬 개발 전용 계정**입니다.

---

## 구조

```
MMOSERVER_Termproject/
  MMOSERVER_Termproject.cpp   IOCP 워커 · 세션 · 패킷 처리 · 시야(AOI) · 전투
  protocol_2026.h             패킷 정의 (과제에서 제공)
  Core/
    TimerManager              8샤드 min-heap 타이머
    Db/                       IDbBackend · SqlServerOdbcBackend · JsonFileBackend · DbWorker
    LuaVM · NpcSpawner · AStar · Map · World · Item · Quest · ObjectPool
CLIENT/                       SFML 클라이언트
STRESS_TEST/                  부하 테스트 클라이언트 (그래픽 / 헤드리스)
data/                         게임 데이터 · NPC AI(Lua) · DB 설정 · 스키마
Resource/                     클라이언트 스프라이트 · 타일 · UI · 오디오
Docs/                         기획서 · 발표 자료
```

---

## AI 활용

Claude Code로 개발했습니다. 리팩터링 · DB 마이그레이션 · 성능 개선 커밋 일부는 Claude와 공동 작성으로 표기돼 있습니다.
병목 원인은 프로파일러로 직접 측정해 판단했고, 수정 후에는 같은 조건으로 다시 측정해 반영했습니다.
