# Resource

게임 클라이언트에서 사용하는 이미지/오디오 리소스 모음.

## 폴더 구조

| 폴더 | 용도 |
|---|---|
| `tiles/` | 지면/배경 타일맵. 던전 바닥, 장애물 등 |
| `hero/` | 플레이어 워리어 스프라이트 (walk/attack/death 등) |
| `monsters/` | NPC 스프라이트 (goblin, orc, skeleton, boss 등) |
| `ui/` | HUD/UI 요소 (HP/MP 구슬, EXP 바, 채팅창, 인벤토리 슬롯) |
| `effects/` | 전투/상태 이펙트 (슬래시, 핏방울, 사망, 부활, 레벨업) |
| `audio/bgm/`, `audio/sfx/` | (선택) 배경음악과 효과음 |

## 파일 명명 규칙

```
{대상}-{용도}-{원본해상도}-{레이아웃설명}.png
```

| 부분 | 예시 | 설명 |
|---|---|---|
| 대상 | `hero`, `orc`, `orb-hp` | 누구/무엇 |
| 용도 | `walk`, `attack`, `death`, `idle` | 어떤 상태/액션 |
| 해상도 | `256x256`, `192x256`, `576x96` | 파일 전체 픽셀 (`WxH`) |
| 레이아웃 | `4dir`, `6frames`, `single` | 시트 구성 (없으면 단일 이미지) |

**규칙**:
- 모두 소문자 + kebab-case
- 한국어 파일명 사용 금지 (인코딩 충돌 방지)
- 시트는 방향 수(`4dir`) 또는 프레임 수(`Nframes`)를 반드시 명시

## 현재 등록된 리소스

| 파일 | 크기 | 레이아웃 | 비고 |
|---|---|---|---|
| `tiles/dungeon-tiles-256x256.png` | 256x256 | 4행x4열 (각 64x64) | 행=지역(0:NW 균열석 / 1:SW 혈흔 / 2:NE 벽돌 / 3:SE 룬), 열=variant |
| `hero/hero-walk-256x256-4dir.png` | 256x256 | 4행x4열 (각 64x64) | 행=방향(Down/Left/Right/Up), 열=walk 프레임 0~3 |
| `hero/hero-attack-192x256-4dir.png` | 192x256 | 4행x3열 (각 64x64) | 행=방향, 열=공격 프레임(windup/slash/recovery). Stage 5에서 연결 예정 |

## 클라이언트 빌드 시 처리

런타임에는 `.exe` 옆에 평탄(flat) 파일로 존재해야 SFML이 로드합니다. `CLIENT/x64/Release/`에 동일 파일을 복사해두면 됨.

클라이언트 코드(`client_sample/client.cpp`)는 다음 경로를 fallback으로 검색하므로 새 리소스 추가 시 같은 패턴을 따르면 됨:
1. `<파일명>` — .exe 옆 (production)
2. `Resource/<subfolder>/<파일명>` — CWD가 프로젝트 루트
3. `../../Resource/<subfolder>/<파일명>` — CWD가 `CLIENT/x64/Release/`
4. `../../../Resource/<subfolder>/<파일명>` — 더 깊은 위치 보호

## 새 리소스 추가 절차

1. 파일을 적절한 서브폴더에 명명 규칙대로 저장
2. `CLIENT/x64/Release/` 에도 같은 이름으로 복사 (또는 빌드 후 자동 복사)
3. `client.cpp`에 텍스처 로드 코드 + fallback 경로 배열 추가
4. `OBJECT::set_xxx()` 같은 헬퍼 추가 (캐릭터/이펙트면)
5. 이 README의 "현재 등록된 리소스" 표에 한 줄 추가
