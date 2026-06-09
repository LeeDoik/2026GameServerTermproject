#define _CRT_SECURE_NO_WARNINGS
#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <SFML/Audio.hpp>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <vector>
using namespace std;

static constexpr float PI_F = 3.14159265358979323846f;

// 서버의 PLAYER_MOVE_INTERVAL_MS와 동일 (0.5초/칸). 키 꾹 누름으로 인한 OS 키 반복이
// 그대로 send되면 서버가 reject해 desync가 발생하므로 클라에서도 동일 쿨타임 적용.
constexpr int CLIENT_MOVE_COOLDOWN_MS = 500;

#include "../../MMOSERVER_Termproject/MMOSERVER_Termproject/protocol_2026.h"

sf::TcpSocket socket;

constexpr auto SCREEN_WIDTH = 16;
constexpr auto SCREEN_HEIGHT = 16;
// Stage 6.4: DCSS 32×32 타일을 2배 스케일로 64px 출력 (정수배 → 보간 없음, 깔끔)
constexpr auto TILE_WIDTH = 64;
constexpr auto WINDOW_WIDTH = SCREEN_WIDTH * TILE_WIDTH;
constexpr auto WINDOW_HEIGHT = SCREEN_HEIGHT * TILE_WIDTH;
constexpr int BUF_SIZE = 1024;

int g_left_x;
int g_top_y;
int g_myid;

// 플레이어 자신의 스탯 (HUD 표시용). S2C_AVATAR_INFO / S2C_StatusChange로 갱신.
int g_my_hp = 100;
int g_my_max_hp = 100;
int g_my_mp = 100;
int g_my_max_mp = 100;
unsigned long long g_my_exp = 0;
unsigned char g_my_level = 1;

// 채팅
bool g_chat_input_mode = false;
std::string g_chat_buffer;
constexpr int CHAT_BUFFER_MAX = MAX_CHAT_MSG_LEN - 1;
constexpr int CHAT_LOG_MAX = 6;  // 화면에 표시할 최근 메시지 수
std::vector<std::string> g_chat_log;

// 내가 마지막에 죽인 NPC 이름 — S2C_StatusChange로 exp가 늘었을 때 "X를 무찔러서 ..." 메시지에 사용
std::string g_last_killed_npc_name;

// 파티 상태
struct PartyMemberInfo {
    int id = 0;
    std::string name;
    int hp = 0;
    int max_hp = 0;
};
std::vector<PartyMemberInfo> g_party_members;   // 자신 제외, 최대 3명
std::unordered_set<int> g_party_member_ids;     // 미니맵 색상 분기용 빠른 조회

// === Stage 8: 아이템 ===
// 카테고리/이름/색은 서버 data/items.txt와 동일 ID로 매핑 (동기화 유지 필요)
enum ClientItemCat { CIC_CONSUMABLE = 0, CIC_WEAPON = 1, CIC_ARMOR = 2 };
// sprite_col: items-dcss.png(256x32, 8 cols, 32x32 each) 시트 column. color는 sprite 로드 실패 시 fallback.
struct ClientItemMeta { const char* name; sf::Color color; int cat; int sprite_col; };
static const std::unordered_map<int, ClientItemMeta> g_item_meta = {
    { 1,  { "Health Potion",       sf::Color(230,  60,  60), CIC_CONSUMABLE, 0 } },
    { 2,  { "Great Health Potion", sf::Color(255, 120, 120), CIC_CONSUMABLE, 1 } },
    { 10, { "Rusty Sword",         sf::Color(150, 150, 160), CIC_WEAPON,     2 } },
    { 11, { "Iron Sword",          sf::Color(120, 160, 210), CIC_WEAPON,     3 } },
    { 12, { "Mythril Blade",       sf::Color(120, 230, 210), CIC_WEAPON,     4 } },
    { 20, { "Leather Armor",       sf::Color(170, 120,  70), CIC_ARMOR,      5 } },
    { 21, { "Chain Mail",          sf::Color(180, 180, 190), CIC_ARMOR,      6 } },
    { 22, { "Plate Armor",         sf::Color(220, 200, 120), CIC_ARMOR,      7 } },
};
static const ClientItemMeta* item_meta(int id) {
    auto it = g_item_meta.find(id);
    return (it == g_item_meta.end()) ? nullptr : &it->second;
}

struct GroundItemView { int item_id; int x; int y; };
std::unordered_map<int, GroundItemView> g_ground_items;  // drop_id → view
std::vector<std::pair<int, int>> g_inventory;            // (item_id, qty) — S2C_Inventory로 갱신
int  g_equipped_weapon_id = -1;
int  g_equipped_armor_id  = -1;
bool g_inv_open = false;
int  g_inv_cursor = 0;   // 인벤토리 커서 슬롯 (0 .. MAX_INVENTORY_SLOTS-1). 방향키로 이동, F/Enter로 사용·장착

// === Stage 9: 퀘스트 ===
// 제목/설명은 서버 data/quests.txt와 동일 ID로 매핑 (서버는 로직, 클라는 표시 텍스트).
struct ClientQuestMeta { const char* title; const char* desc; const char* target_disp; int target_count; };
static const std::unordered_map<int, ClientQuestMeta> g_quest_meta = {
    { 1, { "초원의 군주", "SW 초원의 보스 슬라임 킹을 처치하고 장로에게 보고하세요.",   "GrassBoss",  1 } },
    { 2, { "숲의 수호자", "SE 숲의 보스 히드라를 처치하고 장로에게 보고하세요.",        "ForestBoss", 1 } },
    { 3, { "사막의 폭군", "NE 사막의 보스 황금 드래곤을 처치하고 장로에게 보고하세요.", "DesertBoss", 1 } },
    { 4, { "설원의 마왕", "NW 설원의 보스 케레보브를 처치하고 장로에게 보고하세요.",     "IceBoss",    1 } },
};
static const ClientQuestMeta* quest_meta(int id) {
    auto it = g_quest_meta.find(id);
    return (it == g_quest_meta.end()) ? nullptr : &it->second;
}

struct QuestProgressC { int id; int kill_count; int target; int state; }; // state 0=active,1=completed
std::vector<QuestProgressC> g_quests;          // 보유 퀘스트 (진행+완료)
bool g_quest_log_open = false;                  // J 토글

// 대화창 상태 (S2C_QUEST_DIALOGUE로 세팅)
bool          g_dialogue_open = false;
int           g_dialogue_quest = -1;
unsigned char g_dialogue_kind = 3;             // 0=Offer 1=InProgress 2=ReadyTurnIn 3=None
unsigned char g_dialogue_npc = 0;

// 장로(g_village_npcs[0])에게 받을 수 있는/완료 가능한 퀘스트가 있으면 머리 위 마커 강조용
static bool quest_has_turnin_ready() {
    for (const auto& q : g_quests) {
        if (q.state == 0 && q.kill_count >= q.target) return true;
    }
    return false;
}

static void add_chat_line(const std::string& s) {
    g_chat_log.push_back(s);
    if ((int)g_chat_log.size() > CHAT_LOG_MAX) {
        g_chat_log.erase(g_chat_log.begin(),
                         g_chat_log.begin() + ((int)g_chat_log.size() - CHAT_LOG_MAX));
    }
}

sf::RenderWindow* g_window;
sf::Font* g_font;

// 효과음 (스킬 Q/W/E + 공격/레벨업/피격/줍기)
sf::SoundBuffer g_sfx_q_buf, g_sfx_w_buf, g_sfx_e_buf;
sf::Sound       g_sfx_q,     g_sfx_w,     g_sfx_e;
sf::SoundBuffer g_sfx_a_buf, g_sfx_levelup_buf, g_sfx_hurt_buf, g_sfx_pickup_buf;
sf::Sound       g_sfx_a,     g_sfx_levelup,     g_sfx_hurt,     g_sfx_pickup;

// 워리어 스프라이트 시트 레이아웃 (256x256, 각 셀 64x64)
//   Row 0: walk DOWN  (↓)  cols 0~3 = walk cycle
//   Row 1: walk LEFT  (←)
//   Row 2: walk RIGHT (→)
//   Row 3: walk UP    (↑)
constexpr int HERO_TILE = 64;
constexpr int HERO_DIR_DOWN  = 0;
constexpr int HERO_DIR_LEFT  = 1;
constexpr int HERO_DIR_RIGHT = 2;
constexpr int HERO_DIR_UP    = 3;
constexpr int HERO_IDLE_TIMEOUT_MS = 700;  // 마지막 이동 후 이 시간 지나면 idle 프레임으로
constexpr int HERO_ATTACK_DUR_MS   = 300;  // 공격 모션 총 시간 (3프레임)

class OBJECT {
private:
    bool m_showing;
    sf::Sprite m_sprite;
    sf::Text m_name;

    // 워리어용 애니메이션 상태 (is_hero == true 일 때만 사용)
    bool m_is_hero = false;
    int m_frame_idx = 0;       // 0~3, walk cycle
    sf::Clock m_idle_timer;    // 마지막 이동 이후 경과 시간

    // 공격 모션 (워리어 전용, attack_tex가 있을 때만)
    sf::Texture* m_walk_tex = nullptr;
    sf::Texture* m_attack_tex = nullptr;
    bool m_attacking = false;
    int m_attack_dir = HERO_DIR_DOWN;
    sf::Clock m_attack_clock;

    float m_boss_scale = 1.0f;  // 보스는 5.0f (표시 크기 = HERO_TILE * 5 = 320px)
    int  m_boss_src_px = HERO_TILE;  // 보스 원본 셀 크기 (DCSS 시트 = 32). 실제 스케일 = 표시px / 원본px
    bool m_is_npc = false;

public:
    int m_x, m_y;
    int m_hp = 0, m_max_hp = 0;
    int m_direction = HERO_DIR_DOWN;   // 미니맵 방향 화살표용 — public 노출
    char name[MAX_NAME_LEN];
    OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
        m_showing = false;
        m_x = m_y = -1;
        m_sprite.setTexture(t);
        m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
    }
    OBJECT() {
        m_showing = false;
        m_x = m_y = -1;
    }
    void show() { m_showing = true; }
    void hide() { m_showing = false; }

    void a_move(int x, int y) { m_sprite.setPosition((float)x, (float)y); }
    void a_draw() { g_window->draw(m_sprite); }
    // 위치만 즉시 갱신 (초기 스폰/텔레포트 — 애니메이션 진행 X)
    void move(int x, int y) { m_x = x; m_y = y; }

    // 워리어/오크 등 4방향 walk 스프라이트로 전환. attack_tex가 있으면 공격 모션도 가능.
    void set_hero(sf::Texture& walk_t, sf::Texture* attack_t = nullptr) {
        m_is_hero = true;
        m_walk_tex = &walk_t;
        m_attack_tex = attack_t;
        m_sprite.setTexture(walk_t);
        m_sprite.setTextureRect(sf::IntRect(0, 0, HERO_TILE, HERO_TILE));
        m_sprite.setScale(1.0f, 1.0f);  // 워리어 시트는 64x64 셀 → 화면 64px와 1:1
    }

    // Stage 6.4: DCSS NPC sprite (32x32 단일 셀, 방향/애니메이션 없음). sheet의 col로 선택.
    void set_npc(sf::Texture& sheet, int col) {
        m_is_hero = false;
        m_boss_scale = 1.0f;
        m_is_npc = true;
        m_walk_tex = nullptr;
        m_attack_tex = nullptr;
        m_sprite.setTexture(sheet);
        m_sprite.setTextureRect(sf::IntRect(col * 32, 0, 32, 32));
        m_sprite.setScale((float)TILE_WIDTH / 32.0f, (float)TILE_WIDTH / 32.0f);
    }

    // 보스 전용: DCSS 보스 시트(bosses-dcss.png, 128x32, 4 cols)에서 col을 골라 대형 렌더링.
    // col: 0=Slime King(SW) / 1=Hydra(SE) / 2=Golden Dragon(NE) / 3=Cerebov(NW)
    // DCSS 보스는 정적 32x32 스프라이트(4방향 walk 시트가 아님) → m_is_hero=false 로 애니메이션 비활성.
    void set_boss(sf::Texture& sheet, int col) {
        m_is_hero = false;          // 정적 스프라이트 (방향/walk 프레임 없음)
        m_is_npc = true;            // HP 바 표시
        m_boss_scale = 5.0f;        // 레이아웃 기준 표시 크기 = HERO_TILE * 5 = 320px
        m_boss_src_px = 32;         // DCSS 원본 셀 크기
        m_walk_tex = nullptr;
        m_attack_tex = nullptr;
        m_sprite.setTexture(sheet);
        m_sprite.setTextureRect(sf::IntRect(col * 32, 0, 32, 32));
        float s = (HERO_TILE * m_boss_scale) / (float)m_boss_src_px;  // 320 / 32 = 10
        m_sprite.setScale(s, s);
    }

    // 공격 모션 트리거. direction은 0~3 (Down/Left/Right/Up).
    void on_attack(int direction) {
        if (!m_is_hero || m_attack_tex == nullptr) return;
        m_attacking = true;
        m_attack_dir = direction;
        m_direction = direction;  // 공격 방향으로 캐릭터도 회전 (이동 안 했어도)
        m_attack_clock.restart();
    }

    // 이동 패킷 수신 시: 방향 결정 + walk frame 진행 + 위치 갱신
    void on_move(int new_x, int new_y) {
        if (m_is_hero && m_x != -1) {
            int dx = new_x - m_x;
            int dy = new_y - m_y;
            // 텔레포트(거리 1 초과)는 방향만 갱신, 프레임은 진행 안 함
            bool is_step = (std::abs(dx) <= 1 && std::abs(dy) <= 1);
            if (std::abs(dx) >= std::abs(dy)) {
                if (dx > 0) m_direction = HERO_DIR_RIGHT;
                else if (dx < 0) m_direction = HERO_DIR_LEFT;
            }
            else {
                if (dy > 0) m_direction = HERO_DIR_DOWN;
                else if (dy < 0) m_direction = HERO_DIR_UP;
            }
            if (is_step) {
                m_frame_idx = (m_frame_idx + 1) % 4;
                m_idle_timer.restart();
            }
        }
        m_x = new_x;
        m_y = new_y;
    }

    void draw() {
        if (false == m_showing) return;

        if (m_x < g_left_x || m_x >= g_left_x + SCREEN_WIDTH ||
            m_y < g_top_y || m_y >= g_top_y + SCREEN_HEIGHT) return;

        if (m_is_hero) {
            // 공격 모션 진행 중인지 확인
            if (m_attacking && m_attack_tex != nullptr) {
                long long elapsed = m_attack_clock.getElapsedTime().asMilliseconds();
                if (elapsed >= HERO_ATTACK_DUR_MS) {
                    m_attacking = false;  // 끝났으면 walk 모드로 복귀
                }
                else {
                    int frame = (int)(elapsed * 3 / HERO_ATTACK_DUR_MS);
                    if (frame > 2) frame = 2;
                    m_sprite.setTexture(*m_attack_tex);
                    m_sprite.setTextureRect(sf::IntRect(
                        frame * HERO_TILE, m_attack_dir * HERO_TILE,
                        HERO_TILE, HERO_TILE));
                }
            }
            if (!m_attacking) {
                // 일반 walk 애니메이션
                if (m_walk_tex != nullptr) m_sprite.setTexture(*m_walk_tex);
                int frame = (m_idle_timer.getElapsedTime().asMilliseconds() > HERO_IDLE_TIMEOUT_MS)
                    ? 0 : m_frame_idx;
                m_sprite.setTextureRect(sf::IntRect(
                    frame * HERO_TILE, m_direction * HERO_TILE,
                    HERO_TILE, HERO_TILE));
            }
        }

        float rx = (m_x - g_left_x) * (float)TILE_WIDTH;
        float ry = (m_y - g_top_y) * (float)TILE_WIDTH;

        // 보스는 스케일 유지 + 타일 중앙 정렬 (320px 스프라이트를 64px 타일 기준 중앙에 배치)
        if (m_boss_scale > 1.0f) {
            float sprite_px = HERO_TILE * m_boss_scale;          // 표시 크기 = 320px
            m_sprite.setScale(sprite_px / (float)m_boss_src_px,  // 원본 32px → 10배 / 64px → 5배
                              sprite_px / (float)m_boss_src_px);
            float offset = (sprite_px - (float)TILE_WIDTH) * 0.5f;
            rx -= offset;
            ry -= offset;
        }

        m_sprite.setPosition(rx, ry);
        g_window->draw(m_sprite);
        auto size = m_name.getGlobalBounds();
        // 보스 이름은 스프라이트 상단 중앙
        float name_cx = rx + (m_boss_scale > 1.0f ? HERO_TILE * m_boss_scale * 0.5f : 32.0f);
        m_name.setPosition(name_cx - size.width / 2, ry - 14);
        g_window->draw(m_name);

        // NPC/보스 HP 바
        if (m_is_npc && m_max_hp > 0) {
            float ratio = std::max(0.0f, std::min(1.0f, (float)m_hp / (float)m_max_hp));
            float bar_w = (m_boss_scale > 1.0f) ? HERO_TILE * m_boss_scale : 48.0f;
            float bar_h = (m_boss_scale > 1.0f) ? 8.0f : 5.0f;
            float bar_x = rx + (m_boss_scale > 1.0f ? 0.0f : ((float)TILE_WIDTH - bar_w) / 2.0f);
            float bar_y = ry - 22.0f;

            sf::RectangleShape bg(sf::Vector2f(bar_w, bar_h));
            bg.setFillColor(sf::Color(30, 30, 30, 200));
            bg.setPosition(bar_x, bar_y);
            g_window->draw(bg);

            sf::Color hp_col = (ratio > 0.6f) ? sf::Color(50, 200, 50)
                             : (ratio > 0.3f) ? sf::Color(220, 180, 30)
                                              : sf::Color(220, 50, 50);
            sf::RectangleShape fill(sf::Vector2f(bar_w * ratio, bar_h));
            fill.setFillColor(hp_col);
            fill.setPosition(bar_x, bar_y);
            g_window->draw(fill);

            // 보스만 수치 텍스트 표시
            if (m_boss_scale > 1.0f) {
                sf::Text hp_txt;
                hp_txt.setFont(*g_font);
                char buf[32];
                sprintf_s(buf, "%d / %d", m_hp, m_max_hp);
                hp_txt.setString(buf);
                hp_txt.setCharacterSize(14);
                hp_txt.setFillColor(sf::Color::White);
                hp_txt.setOutlineColor(sf::Color::Black);
                hp_txt.setOutlineThickness(1.5f);
                sf::FloatRect b = hp_txt.getLocalBounds();
                hp_txt.setOrigin(b.left + b.width / 2.0f, b.top + b.height / 2.0f);
                hp_txt.setPosition(bar_x + bar_w / 2.0f, bar_y - 12.0f);
                g_window->draw(hp_txt);
            }
        }
    }

    void set_name(const char str[]) {
        strncpy_s(name, sizeof(name), str, _TRUNCATE);
        m_name.setFont(*g_font);
        m_name.setString(str);
        m_name.setCharacterSize(20);
        m_name.setFillColor(sf::Color(80, 220, 80));
        m_name.setStyle(sf::Text::Bold);
    }
};

OBJECT avatar;
std::unordered_map <int, OBJECT> players;
std::string avatar_name;

// 던전 타일맵 (Stage 6.4 DCSS): 128x128 PNG, 4행x4열, 각 32x32. setScale 2.0으로 64px 출력.
// 행 = 지역 (0:NW설원 / 1:SW초원 / 2:NE사막 / 3:SE숲), 열 = 같은 지역의 4가지 variant.
sf::Texture* dungeon_tiles;
sf::Sprite tile_sprite;
constexpr int TILE_SRC_SIZE = 32;          // DCSS 원본 32x32, 화면에선 setScale 2.0으로 64px 출력
constexpr int REGION_HALF   = WORLD_WIDTH / 2;  // 2000/2 = 1000 (지역 경계)

// 장애물 sprite 시트 (Stage 6.4): 160x32 PNG, 5열 (NW crystal / SW tree_1_yellow / NE sandstone / SE tree / Village brick), 각 32x32
sf::Texture* obstacle_tex;
sf::Sprite obstacle_sprite;

// === Stage 6.1: 장애물 시각화 ===
// 서버 obstacles.txt와 동일한 데이터를 클라도 로드해서 흰 사각형으로 표시 (임시 디자인)
constexpr size_t OBSTACLE_BITMAP_BYTES = (size_t(WORLD_WIDTH) * WORLD_HEIGHT + 7) / 8;
std::vector<unsigned char> g_obstacle_bits;
sf::RectangleShape g_wall_shape;

// === Aetheria Village (시작 마을) ===
// 기획서 "평원(중앙) + 시작 안전지대" 명세 기반. 80x80 영역, 외벽은 obstacles.txt에 정의.
constexpr int VILLAGE_X1 = 960;
constexpr int VILLAGE_Y1 = 960;
constexpr int VILLAGE_X2 = 1040;
constexpr int VILLAGE_Y2 = 1040;
constexpr int FOUNTAIN_X = 1000;
constexpr int FOUNTAIN_Y = 1000;

sf::RectangleShape g_village_floor;  // 베이지 반투명 바닥

// Stage 6.4 마을 데코 시트: 256x32 (8열×1행, 32×32 each)
// col 0=fountain, 1=statue, 2=wizard(장로), 3=dwarf(무기), 4=human(방어구), 5=halfling(포션),
//     6=gate_left, 7=gate_right
sf::Texture* village_deco_tex;
sf::Sprite   village_deco_sprite;

// 마을 NPC (직업별 sprite + 머리 위 마커)
struct VillageNpcMarker { int x, y; int sprite_col; const char* label; };
const VillageNpcMarker g_village_npcs[] = {
    {  985,  985, 2, "!" },   // 장로 (퀘스트)
    { 1015,  985, 3, "$" },   // 무기 상인
    {  985, 1015, 4, "$" },   // 방어구 상인
    { 1015, 1015, 5, "$" },   // 포션 상인
};

// 마을 모서리 동상
struct VillageStatue { int x, y; };
const VillageStatue g_village_statues[] = {
    {  965,  965 }, { 1035,  965 }, {  965, 1035 }, { 1035, 1035 },
};

// Stage 6.4: 4지역 랜드마크/보스/장식 — RPG 진행 흐름 (마을 → SW → SE → NE → NW)
struct WorldDeco {
    int x, y;
    int sprite_col;         // landmarks-dcss.png 시트 인덱스 (0~11)
    const char* label;      // "" 이면 라벨 없음
    bool is_boss;           // 미니맵에 큰 빨간 점으로 표시
};

// 거점 + 보스존 (각 지역 입구쪽 거점 + 깊은 곳 보스)
const WorldDeco g_landmarks[] = {
    // SW 초원 (Lv 1-10)
    {  500, 1200,  0, "Slime Camp [Lv 1-10]",      false },
    {  300, 1700,  4, "[BOSS] Slime King",         true  },
    // SE 숲 (Lv 10-20)
    { 1500, 1200,  1, "Sacred Grove [Lv 10-20]",   false },
    { 1700, 1700,  5, "[BOSS] Hydra",              true  },
    // NE 사막 (Lv 20-30)
    { 1500,  800,  2, "Pyramid Ruin [Lv 20-30]",   false },
    { 1700,  300,  6, "[BOSS] Golden Dragon",      true  },
    // NW 설원 (Lv 30-40)
    {  500,  800,  3, "Frozen Shrine [Lv 30-40]",  false },
    {  300,  300,  7, "[BOSS] Cerebov",            true  },
};

// 흩어진 장식 (라벨 없음, 시각만)
const WorldDeco g_decorations[] = {
    // SW 꽃밭 (col 10)
    {  200, 1100, 10, "", false }, {  600, 1300, 10, "", false },
    {  400, 1600, 10, "", false }, {  800, 1800, 10, "", false },
    // SE 큰 나무 (col 11)
    { 1200, 1200, 11, "", false }, { 1700, 1400, 11, "", false },
    { 1300, 1700, 11, "", false }, { 1900, 1500, 11, "", false },
    // NE 부서진 기둥 (col 9)
    { 1200,  200,  9, "", false }, { 1600,  500,  9, "", false },
    { 1900,  300,  9, "", false }, { 1400,  700,  9, "", false },
    // NW 얼음 결정 (col 8)
    {  200,  200,  8, "", false }, {  700,  100,  8, "", false },
    {  400,  500,  8, "", false }, {  800,  700,  8, "", false },
};

// obstacles.txt에 정의된 명시적 장애물 (rect 기반)
static bool client_is_obstacle_bit(int x, int y) {
    if (g_obstacle_bits.empty()) return false;
    if (x < 0 || y < 0 || x >= WORLD_WIDTH || y >= WORLD_HEIGHT) return false;
    size_t idx = size_t(y) * WORLD_WIDTH + size_t(x);
    return ((g_obstacle_bits[idx >> 3] >> (idx & 7)) & 1u) != 0;
}

// Stage 6.4: 절차적 장식 (서버 Map::IsProceduralDeco와 동일 해시).
// 마을 영역 제외, 하위 8비트 < 12면 deco. deco도 실제 장애물로 취급.
static bool client_is_procedural_deco(int x, int y) {
    if (x < 0 || y < 0 || x >= WORLD_WIDTH || y >= WORLD_HEIGHT) return false;
    if (x >= VILLAGE_X1 && x < VILLAGE_X2 && y >= VILLAGE_Y1 && y < VILLAGE_Y2) return false;
    unsigned int dh = (unsigned int)(x * 0x9E3779B1u) ^ (unsigned int)(y * 0x85EBCA77u);
    return (dh & 0xFFu) < 12u;
}

// 종합 장애물 (obstacle bits OR 절차적 deco)
static bool client_is_blocked(int x, int y) {
    return client_is_obstacle_bit(x, y) || client_is_procedural_deco(x, y);
}

static bool load_obstacles_file(const char* path) {
    FILE* fp = std::fopen(path, "r");
    if (!fp) return false;
    g_obstacle_bits.assign(OBSTACLE_BITMAP_BYTES, 0);
    int rect_count = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char kind[16];
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (std::sscanf(p, "%15s %d %d %d %d", kind, &x1, &y1, &x2, &y2) != 5) continue;
        if (_stricmp(kind, "rect") != 0) continue;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > WORLD_WIDTH)  x2 = WORLD_WIDTH;
        if (y2 > WORLD_HEIGHT) y2 = WORLD_HEIGHT;
        if (x2 <= x1 || y2 <= y1) continue;
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) {
                size_t idx = size_t(y) * WORLD_WIDTH + size_t(x);
                g_obstacle_bits[idx >> 3] |= (unsigned char)(1u << (idx & 7));
            }
        }
        ++rect_count;
    }
    std::fclose(fp);
    cout << "[Client] obstacles loaded from " << path << ": " << rect_count << " rects\n";
    return true;
}

// 4방향 walk 스프라이트 시트 (모두 동일 레이아웃: 256x256, Row=방향, Col=walk프레임)
sf::Texture* hero_tex;        // 플레이어 워리어 walk
sf::Texture* hero_attack_tex; // 플레이어 워리어 attack (192x256, 4행x3열, 각 64x64)
sf::Texture* orc_tex;         // NPC: Agro Orc (fallback — visual_id 범위 밖일 때만)
sf::Texture* npcs_tex;        // Stage 6.4: DCSS NPC 시트 (512x32, 16 cols, 32×32 each, visual_id 1~16)
sf::Texture* bosses_tex;      // DCSS 보스 시트 (128x32, 4 cols, 32×32 each, visual_id 17~20)
                              //   col 0=Slime King(SW) / 1=Hydra(SE) / 2=Golden Dragon(NE) / 3=Cerebov(NW)
sf::Texture* landmarks_tex;   // Stage 6.4: 랜드마크+보스+장식 시트 (384x32, 12 cols)
sf::Texture* items_tex;       // Stage 8: DCSS 아이템 시트 (256x32, 8 cols, 32×32). ClientItemMeta.sprite_col로 선택
sf::Sprite   landmark_sprite;

// 아이템 스프라이트를 (cx,cy) 중심에 한 변 size 픽셀로 그린다 (바닥 드롭 + 인벤토리 공용).
// meta/텍스처 없으면 기존 회전 사각형(gem)으로 fallback.
static void draw_item_sprite(int item_id, float cx, float cy, float size) {
    const ClientItemMeta* m = item_meta(item_id);
    if (m && items_tex) {
        sf::Sprite s;
        s.setTexture(*items_tex);
        s.setTextureRect(sf::IntRect(m->sprite_col * 32, 0, 32, 32));
        s.setOrigin(16.0f, 16.0f);
        s.setScale(size / 32.0f, size / 32.0f);
        s.setPosition(cx, cy);
        g_window->draw(s);
    } else {
        sf::Color col = m ? m->color : sf::Color(220, 220, 220);
        sf::RectangleShape gem(sf::Vector2f(size * 0.85f, size * 0.85f));
        gem.setOrigin(gem.getSize().x * 0.5f, gem.getSize().y * 0.5f);
        gem.setPosition(cx, cy);
        gem.setRotation(45.0f);
        gem.setFillColor(col);
        gem.setOutlineColor(sf::Color(20, 20, 20));
        gem.setOutlineThickness(2.0f);
        g_window->draw(gem);
    }
}

// HUD 리소스
sf::Texture* orb_tex;    // 512x256 (HP구슬 좌측 256, MP구슬 우측 256)
sf::Texture* exp_frame_tex;  // 1024x48 EXP 바 프레임
sf::Texture* exp_fill_tex;   // 16x32 EXP 바 채움 strip
sf::Texture* chat_panel_tex; // 512x192 채팅 패널

// 이펙트 시트 (frame_count, frame_w, frame_h 정보는 Effect spawn에서 지정)
sf::Texture* blood_tex;      // 320x64,  5 프레임 64x64  (혈흔)
sf::Texture* death_tex;      // 768x128, 8 프레임 96x128 (사망 영혼)
sf::Texture* respawn_tex;    // 576x160, 6 프레임 96x160 (리스폰 빛기둥)
sf::Texture* levelup_tex;    // 896x128, 7 프레임 128x128 (레벨업 버스트)
sf::Texture* slash_tex;      // 576x96,  6 프레임 96x96  (검 슬래시)

// 스킬 이펙트 시트 (DCSS 32x32, scale=2.0으로 64px 출력)
sf::Texture* skill_aoe_tex;     // 96x32,  3 프레임 — cloud_fire (AoE 화염)
sf::Texture* skill_line_tex;    // 192x32, 6 프레임 — searing_ray (Line 관통빔)
sf::Texture* skill_heal_tex;    // 96x32,  3 프레임 — goldaura (Heal 오라)
sf::Texture* skill_sparkle_tex; // 96x32,  3 프레임 — gold_sparkles (Heal 스파클)

// 스킬 슬롯 아이콘 (DCSS gui 32x32)
sf::Texture* skill_icon_q_tex;  // Q: AoE 화염 아이콘 (qazlal_upheaval)
sf::Texture* skill_icon_w_tex;  // W: Line 관통 아이콘 (dithmenos_shadow_step)
sf::Texture* skill_icon_e_tex;  // E: Heal 아이콘 (elyvilon_heal_other)

// 기본 공격 이펙트
sf::Texture* attack_player_tex;        // (구) 128x32, 4 프레임 — zap (미사용, 호환용)
sf::Texture* attack_player_impact_tex; // 160x32, 5 프레임 — sfx-Sheet1 Row0 임팩트 버스트
sf::Texture* attack_npc_tex;           // 96x32,  3 프레임 — sandblast (NPC 정면 1칸)

sf::Sprite hud_sprite;       // HUD 그리기용 재사용 sprite

// 화면에 떠 있는 이펙트 1개. 시트 한 행을 프레임 순서대로 재생 후 자동 소멸.
// start_delay_ms로 spawn 시점부터 일정 시간 대기 후 시작 가능 (폭발 퍼짐 효과용).
struct Effect {
    sf::Sprite sprite;
    int frame_count;
    int frame_w, frame_h;
    int duration_ms;
    int world_x, world_y;     // 월드 타일 좌표
    int offset_x, offset_y;    // 그릴 때 타일 좌상단 기준 픽셀 보정
    float scale = 1.0f;        // 렌더 배율 (DCSS 32x32 → 2.0f로 64x64)
    int start_delay_ms = 0;    // 이 시간 지나기 전엔 보이지 않음
    sf::Color tint{ 255, 255, 255, 255 };  // 스프라이트 색상 틴트
    sf::Clock clock;

    long long active_ms() const {
        return clock.getElapsedTime().asMilliseconds() - start_delay_ms;
    }
    bool is_done() const {
        return active_ms() >= duration_ms;
    }
    int current_frame() {
        long long ms = active_ms();
        int f = (int)(ms * frame_count / duration_ms);
        if (f < 0) f = 0;
        if (f >= frame_count) f = frame_count - 1;
        return f;
    }
    void draw() {
        // 시야 밖이면 스킵 (이펙트는 진행하되 화면에는 안 그림)
        if (world_x < g_left_x || world_x >= g_left_x + SCREEN_WIDTH ||
            world_y < g_top_y || world_y >= g_top_y + SCREEN_HEIGHT) return;
        // 시작 지연 중이면 안 그림 (delay queue 없이 spawn 큐를 통합)
        if (active_ms() < 0) return;
        int f = current_frame();
        sprite.setTextureRect(sf::IntRect(f * frame_w, 0, frame_w, frame_h));
        sprite.setScale(scale, scale);
        sprite.setColor(tint);
        float rx = (world_x - g_left_x) * (float)TILE_WIDTH + offset_x;
        float ry = (world_y - g_top_y) * (float)TILE_WIDTH + offset_y;
        sprite.setPosition(rx, ry);
        g_window->draw(sprite);
    }
};

std::vector<Effect> g_effects;

// 이펙트 4종 spawn 헬퍼. 같은 함수가 Phase 2~4에서 패킷 수신 시에도 호출됨.
static void spawn_effect_blood(int wx, int wy) {
    Effect e;
    e.sprite.setTexture(*blood_tex);
    e.frame_count = 5;
    e.frame_w = 64; e.frame_h = 64;
    e.duration_ms = 500;
    e.world_x = wx; e.world_y = wy;
    // 64x64를 65px 타일 안에 거의 그대로 가운데 정렬
    e.offset_x = (TILE_WIDTH - 64) / 2;
    e.offset_y = (TILE_WIDTH - 64) / 2;
    g_effects.push_back(std::move(e));
}
static void spawn_effect_death(int wx, int wy) {
    Effect e;
    e.sprite.setTexture(*death_tex);
    e.frame_count = 8;
    e.frame_w = 96; e.frame_h = 128;
    e.duration_ms = 1200;
    e.world_x = wx; e.world_y = wy;
    // 가로 중앙 정렬 + 발 위치 기준 (영혼이 위로 떠오르도록 타일 발쪽에 앵커)
    e.offset_x = (TILE_WIDTH - 96) / 2;
    e.offset_y = TILE_WIDTH - 128;  // 약 -63 (위로 늘어남)
    g_effects.push_back(std::move(e));
}
static void spawn_effect_respawn(int wx, int wy) {
    Effect e;
    e.sprite.setTexture(*respawn_tex);
    e.frame_count = 6;
    e.frame_w = 96; e.frame_h = 160;
    e.duration_ms = 1000;
    e.world_x = wx; e.world_y = wy;
    // 빛기둥이 위에서 내려오는 느낌 — 가로 중앙, 발쪽 앵커
    e.offset_x = (TILE_WIDTH - 96) / 2;
    e.offset_y = TILE_WIDTH - 160;  // 약 -95
    g_effects.push_back(std::move(e));
}
static void spawn_effect_levelup(int wx, int wy) {
    Effect e;
    e.sprite.setTexture(*levelup_tex);
    e.frame_count = 7;
    e.frame_w = 128; e.frame_h = 128;
    e.duration_ms = 1000;
    e.world_x = wx; e.world_y = wy;
    // 128x128을 65px 타일 가운데 정렬 (음수 offset — 타일 밖으로 퍼짐)
    e.offset_x = (TILE_WIDTH - 128) / 2;
    e.offset_y = (TILE_WIDTH - 128) / 2;
    g_effects.push_back(std::move(e));
}
// 슬래시: 공격 모션 중 무기 궤적. duration 짧게 (공격 prep+swing이 ~300ms).
static void spawn_effect_slash(int wx, int wy) {
    Effect e;
    e.sprite.setTexture(*slash_tex);
    e.frame_count = 6;
    e.frame_w = 96; e.frame_h = 96;
    e.duration_ms = 350;
    e.world_x = wx; e.world_y = wy;
    // 96x96을 65px 타일 가운데 정렬
    e.offset_x = (TILE_WIDTH - 96) / 2;
    e.offset_y = (TILE_WIDTH - 96) / 2;
    g_effects.push_back(std::move(e));
}

// DCSS 32x32 시트를 scale=2.0으로 64px 출력하는 헬퍼 (offset은 자동 중앙 정렬)
static Effect make_dcss_effect(sf::Texture* tex, int frame_count, int duration_ms, int wx, int wy) {
    Effect e;
    e.sprite.setTexture(*tex);
    e.frame_count = frame_count;
    e.frame_w = 32; e.frame_h = 32;
    e.duration_ms = duration_ms;
    e.world_x = wx; e.world_y = wy;
    e.scale = 2.0f;  // 32x32 × 2 = 64x64 (TILE_WIDTH와 일치)
    e.offset_x = 0;  // 64px 타일에 64px 이펙트 → offset 0
    e.offset_y = 0;
    return e;
}

// 스킬 1 AoE: cloud_fire로 시전자 중심 5x5 정사각형 25타일 전체 화염 이펙트
// 서버 SKILL_AOE_RANGE=2 (chebyshev) → dx,dy ∈ [-2,2] 모두 데미지 → 이펙트도 동일 영역
// 중심에서 거리(chebyshev)에 비례한 시작 지연으로 폭발이 외곽으로 퍼지는 느낌
static void spawn_effect_skill_aoe(int wx, int wy) {
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int cheb = std::max(std::abs(dx), std::abs(dy));
            // 중심=800ms, 외곽으로 갈수록 짧고 늦게 — 코어가 가장 진하고 가장자리는 잔불 느낌
            int dur   = 700 - cheb * 100;   // 700 / 600 / 500ms
            int delay = cheb * 100;          // 0 / 100 / 200ms 지연
            Effect e = make_dcss_effect(skill_aoe_tex, 3, dur, wx + dx, wy + dy);
            e.start_delay_ms = delay;
            g_effects.push_back(std::move(e));
        }
    }
}

// 스킬 2 Line: searing_ray를 방향 5칸에 연속 spawn
static void spawn_effect_skill_line(int wx, int wy, int dir) {
    // dir: 0=Down(dy+1), 1=Left(dx-1), 2=Right(dx+1), 3=Up(dy-1)
    int ddx = 0, ddy = 0;
    switch (dir) { case 0: ddy=1; break; case 1: ddx=-1; break; case 2: ddx=1; break; case 3: ddy=-1; break; }
    for (int step = 1; step <= 5; ++step) {
        int tx = wx + ddx * step;
        int ty = wy + ddy * step;
        g_effects.push_back(make_dcss_effect(skill_line_tex, 6, 600, tx, ty));
    }
    // 시전자 발사 지점에도 짧은 이펙트
    g_effects.push_back(make_dcss_effect(skill_line_tex, 6, 300, wx, wy));
}

// 스킬 3 Heal: goldaura + sparkle 겹쳐서 황금빛 치유 표현
static void spawn_effect_skill_heal(int wx, int wy) {
    g_effects.push_back(make_dcss_effect(skill_heal_tex,    3, 900, wx, wy));
    g_effects.push_back(make_dcss_effect(skill_sparkle_tex, 3, 700, wx, wy));
}

// 플레이어 기본 공격: 위→아래 내려치기 슬래시 (sfx-Sheet1 Row5)
static void spawn_effect_attack_player(int wx, int wy) {
    // 시전자 타일에 메인 슬래시 (scale 3.0 → 96px, 청백 날카로운 색)
    {
        Effect e = make_dcss_effect(attack_player_impact_tex, 5, 350, wx, wy);
        e.scale = 3.0f;
        e.offset_x = (TILE_WIDTH - (int)(32 * 3.0f)) / 2;  // 중앙 정렬
        e.offset_y = (TILE_WIDTH - (int)(32 * 3.0f)) / 2;
        e.tint = sf::Color(200, 235, 255);  // 청백 — 날카로운 칼날 느낌
        g_effects.push_back(std::move(e));
    }
    // 인접 4칸에 잔상 슬래시 (scale 2.0, 반투명). 재생속도를 메인과 동일하게 350ms로 통일.
    int offsets[4][2] = {{0,-1},{0,1},{-1,0},{1,0}};
    for (auto& o : offsets) {
        Effect e = make_dcss_effect(attack_player_impact_tex, 5, 350, wx + o[0], wy + o[1]);
        e.tint = sf::Color(180, 220, 255, 140);
        g_effects.push_back(std::move(e));
    }
}

// NPC 기본 공격: sandblast 이펙트를 공격 방향 1칸에만 spawn
// dir: 0=Down(dy+1), 1=Left(dx-1), 2=Right(dx+1), 3=Up(dy-1)
static void spawn_effect_attack_npc(int wx, int wy, int dir) {
    int ddx = 0, ddy = 0;
    switch (dir) { case 0: ddy=1; break; case 1: ddx=-1; break; case 2: ddx=1; break; case 3: ddy=-1; break; }
    g_effects.push_back(make_dcss_effect(attack_npc_tex, 3, 350, wx + ddx, wy + ddy));
}

// ============================================================================
// 화면 흔들기 (피격/사망/보스 AoE 시각화)
// ============================================================================
static sf::Clock g_shake_clock;
static float g_shake_strength_px = 0.0f;
static int g_shake_duration_ms = 0;

static void trigger_shake(float strength, int duration_ms) {
    // 더 강한 흔들기로 덮어씀. 약한 건 무시 (큰 흔들기 중 잔흔들기로 약해지지 않게).
    if (strength > g_shake_strength_px) {
        g_shake_strength_px = strength;
        g_shake_duration_ms = duration_ms;
        g_shake_clock.restart();
    }
}
static sf::Vector2f get_shake_offset() {
    if (g_shake_strength_px <= 0.0f) return { 0.f, 0.f };
    int ms = g_shake_clock.getElapsedTime().asMilliseconds();
    if (ms >= g_shake_duration_ms) { g_shake_strength_px = 0.0f; return { 0.f, 0.f }; }
    float t = (float)ms / (float)g_shake_duration_ms;     // 0..1
    float amp = g_shake_strength_px * (1.0f - t);          // 선형 감쇠
    float fx = std::sin(ms * 0.071f) + 0.5f * std::sin(ms * 0.137f);
    float fy = std::cos(ms * 0.083f) + 0.5f * std::cos(ms * 0.151f);
    return { amp * fx, amp * fy };
}

// ============================================================================
// 데미지 숫자 popup (S2C_DAMAGE 수신 시 떠오르는 텍스트)
// ============================================================================
struct FloatingDamage {
    int wx, wy;
    int value;
    sf::Color color;
    sf::Clock clock;
    int duration_ms = 900;
    float jitter_x = 0.0f;

    bool is_done() const { return clock.getElapsedTime().asMilliseconds() >= duration_ms; }
    void draw() {
        if (wx < g_left_x - 1 || wx >= g_left_x + SCREEN_WIDTH + 1 ||
            wy < g_top_y  - 1 || wy >= g_top_y  + SCREEN_HEIGHT + 1) return;
        int ms = clock.getElapsedTime().asMilliseconds();
        float t = std::min(1.0f, (float)ms / (float)duration_ms);
        float rise = -48.0f * t;
        float alpha = 1.0f;
        if (t > 0.6f) alpha = 1.0f - (t - 0.6f) / 0.4f;
        sf::Color c = color;
        float a255 = alpha * 255.0f;
        if (a255 < 0.0f) a255 = 0.0f; else if (a255 > 255.0f) a255 = 255.0f;
        c.a = (sf::Uint8)a255;
        sf::Text txt;
        txt.setFont(*g_font);
        txt.setString(std::to_string(value));
        txt.setCharacterSize(22);
        txt.setStyle(sf::Text::Bold);
        txt.setFillColor(c);
        txt.setOutlineColor(sf::Color(0, 0, 0, c.a));
        txt.setOutlineThickness(2.0f);
        sf::FloatRect b = txt.getLocalBounds();
        txt.setOrigin(b.left + b.width / 2.0f, b.top + b.height / 2.0f);
        float px = (wx - g_left_x + 0.5f) * (float)TILE_WIDTH + jitter_x;
        float py = (wy - g_top_y)  * (float)TILE_WIDTH + rise;
        txt.setPosition(px, py);
        g_window->draw(txt);
    }
};
std::vector<FloatingDamage> g_dmg_popups;

static void spawn_damage_popup(int wx, int wy, int dmg, sf::Color c) {
    FloatingDamage d;
    d.wx = wx; d.wy = wy;
    d.value = dmg;
    d.color = c;
    d.jitter_x = (float)((std::rand() % 33) - 16);
    g_dmg_popups.push_back(std::move(d));
}

// 스킬 클라이언트 쿨타임 (HUD 표시용)
static sf::Clock g_skill1_clock;   // AoE (Q)
static sf::Clock g_skill2_clock;   // Line (W)
static sf::Clock g_skill3_clock;   // Heal (E)
static bool g_skill1_used = false;
static bool g_skill2_used = false;
static bool g_skill3_used = false;
constexpr int SKILL1_CD_MS = 3000;
constexpr int SKILL2_CD_MS = 3000;
constexpr int SKILL3_CD_MS = 10000;

// 텍스처 로드 헬퍼: 경로 후보를 순서대로 시도해서 처음 성공한 것을 사용
static bool LoadTextureWithFallback(sf::Texture* tex, const char* name,
                                    const char* subfolder)
{
    char p0[256], p1[256], p2[256], p3[256];
    std::snprintf(p0, sizeof(p0), "%s", name);
    std::snprintf(p1, sizeof(p1), "Resource/%s/%s", subfolder, name);
    std::snprintf(p2, sizeof(p2), "../../Resource/%s/%s", subfolder, name);
    std::snprintf(p3, sizeof(p3), "../../../Resource/%s/%s", subfolder, name);
    const char* cands[] = { p0, p1, p2, p3 };
    for (const char* p : cands) {
        if (tex->loadFromFile(p)) {
            cout << "[Client] " << name << " loaded: " << p << "\n";
            return true;
        }
    }
    cout << "Texture Loading Error: " << name << "\n";
    return false;
}

void client_initialize()
{
    dungeon_tiles    = new sf::Texture;
    obstacle_tex     = new sf::Texture;
    village_deco_tex = new sf::Texture;
    npcs_tex         = new sf::Texture;
    bosses_tex       = new sf::Texture;
    items_tex        = new sf::Texture;
    landmarks_tex    = new sf::Texture;
    hero_tex         = new sf::Texture;
    hero_attack_tex = new sf::Texture;
    orc_tex       = new sf::Texture;
    orb_tex       = new sf::Texture;
    exp_frame_tex = new sf::Texture;
    exp_fill_tex  = new sf::Texture;
    chat_panel_tex = new sf::Texture;
    blood_tex     = new sf::Texture;
    death_tex     = new sf::Texture;
    respawn_tex   = new sf::Texture;
    levelup_tex   = new sf::Texture;
    slash_tex        = new sf::Texture;
    skill_aoe_tex    = new sf::Texture;
    skill_line_tex   = new sf::Texture;
    skill_heal_tex   = new sf::Texture;
    skill_sparkle_tex= new sf::Texture;
    attack_player_tex        = new sf::Texture;
    attack_player_impact_tex = new sf::Texture;
    attack_npc_tex           = new sf::Texture;
    skill_icon_q_tex = new sf::Texture;
    skill_icon_w_tex = new sf::Texture;
    skill_icon_e_tex = new sf::Texture;

    if (!LoadTextureWithFallback(dungeon_tiles,  "dungeon-tiles-dcss.png",           "tiles"))     exit(-1);
    if (!LoadTextureWithFallback(obstacle_tex,     "obstacles-dcss.png",             "tiles"))     exit(-1);
    if (!LoadTextureWithFallback(village_deco_tex, "village-deco-dcss.png",          "tiles"))     exit(-1);
    if (!LoadTextureWithFallback(npcs_tex,         "npcs-dcss.png",                  "tiles"))     exit(-1);
    npcs_tex->setSmooth(false);
    if (!LoadTextureWithFallback(bosses_tex,       "bosses-dcss.png",                "tiles"))     exit(-1);
    bosses_tex->setSmooth(false);
    if (!LoadTextureWithFallback(items_tex,        "items-dcss.png",                 "tiles"))     exit(-1);
    items_tex->setSmooth(false);
    if (!LoadTextureWithFallback(landmarks_tex,    "landmarks-dcss.png",             "tiles"))     exit(-1);
    landmarks_tex->setSmooth(false);
    landmark_sprite.setTexture(*landmarks_tex);
    landmark_sprite.setScale((float)TILE_WIDTH / 32.0f, (float)TILE_WIDTH / 32.0f);
    if (!LoadTextureWithFallback(hero_tex,       "hero-walk-256x256-4dir.png",       "hero"))      exit(-1);
    if (!LoadTextureWithFallback(hero_attack_tex,"hero-attack-192x256-4dir.png",     "hero"))      exit(-1);
    if (!LoadTextureWithFallback(orc_tex,        "red-orc-walk-256-4dir.png",        "monsters"))  exit(-1);
    if (!LoadTextureWithFallback(orb_tex,        "hp-mp-orbs-frames-512x256.png",    "ui"))        exit(-1);
    if (!LoadTextureWithFallback(exp_frame_tex,  "exp-bar-frame-1024x48.png",        "ui"))        exit(-1);
    if (!LoadTextureWithFallback(exp_fill_tex,   "exp-bar-fill-tile-16x32.png",      "ui"))        exit(-1);
    if (!LoadTextureWithFallback(chat_panel_tex, "chat-panel-512x192.png",           "ui"))        exit(-1);
    if (!LoadTextureWithFallback(blood_tex,      "blood-splatter-320x64.png",        "effects"))   exit(-1);
    if (!LoadTextureWithFallback(death_tex,      "soul-death-effect-768x128.png",    "effects"))   exit(-1);
    if (!LoadTextureWithFallback(respawn_tex,    "respawn-pillar-576x160.png",       "effects"))   exit(-1);
    if (!LoadTextureWithFallback(levelup_tex,    "level-up-burst-896x128.png",       "effects"))   exit(-1);
    if (!LoadTextureWithFallback(slash_tex,       "slash-effect-576x96.png",          "effects"))   exit(-1);
    if (!LoadTextureWithFallback(skill_aoe_tex,   "skill-aoe-fire-96x32.png",         "effects"))   exit(-1);
    if (!LoadTextureWithFallback(skill_line_tex,  "skill-line-ray-192x32.png",        "effects"))   exit(-1);
    if (!LoadTextureWithFallback(skill_heal_tex,  "skill-heal-gold-96x32.png",        "effects"))   exit(-1);
    if (!LoadTextureWithFallback(skill_sparkle_tex,"skill-heal-sparkle-96x32.png",    "effects"))   exit(-1);
    if (!LoadTextureWithFallback(attack_player_tex,       "attack-player-zap-128x32.png",    "effects"))   exit(-1);
    if (!LoadTextureWithFallback(attack_player_impact_tex,"attack-player-slash-down-160x32.png", "effects"))   exit(-1);
    if (!LoadTextureWithFallback(attack_npc_tex,          "attack-npc-sandblast-96x32.png",  "effects"))   exit(-1);
    if (!LoadTextureWithFallback(skill_icon_q_tex, "skill-icon-q.png",                "ui"))        exit(-1);
    if (!LoadTextureWithFallback(skill_icon_w_tex, "skill-icon-w.png",                "ui"))        exit(-1);
    if (!LoadTextureWithFallback(skill_icon_e_tex, "skill-icon-e.png",                "ui"))        exit(-1);

    tile_sprite.setTexture(*dungeon_tiles);
    // DCSS 32x32 원본을 64px 칸에 정수배(2x) 업스케일. setSmooth(false)로 픽셀아트 보존.
    dungeon_tiles->setSmooth(false);
    tile_sprite.setScale((float)TILE_WIDTH / TILE_SRC_SIZE, (float)TILE_WIDTH / TILE_SRC_SIZE);

    // 장애물 sprite: 32×32 원본을 setScale 2.0으로 64px 출력. 지역별 column 선택.
    obstacle_tex->setSmooth(false);
    obstacle_sprite.setTexture(*obstacle_tex);
    obstacle_sprite.setScale((float)TILE_WIDTH / 32.0f, (float)TILE_WIDTH / 32.0f);
    // 기존 fallback (흰 사각형) — 텍스처 없을 때만 사용. 일반 동작은 sprite 우선.
    g_wall_shape.setSize(sf::Vector2f((float)TILE_WIDTH, (float)TILE_WIDTH));
    g_wall_shape.setFillColor(sf::Color::White);

    // 마을 바닥 (반투명 베이지, 타일당 1개)
    g_village_floor.setSize(sf::Vector2f((float)TILE_WIDTH, (float)TILE_WIDTH));
    g_village_floor.setFillColor(sf::Color(230, 215, 170, 110));

    // 마을 데코 sprite (분수/동상/NPC) — 32×32 원본을 setScale 2.0으로 64px 출력
    village_deco_tex->setSmooth(false);
    village_deco_sprite.setTexture(*village_deco_tex);
    village_deco_sprite.setScale((float)TILE_WIDTH / 32.0f, (float)TILE_WIDTH / 32.0f);

    // 서버와 같은 obstacles.txt를 클라가 읽어 흰 사각형으로 표시
    const char* obstacle_paths[] = {
        "data/obstacles.txt",
        "../../data/obstacles.txt",
        "../../../data/obstacles.txt",
    };
    for (const char* p : obstacle_paths) {
        if (load_obstacles_file(p)) break;
    }
    if (g_obstacle_bits.empty()) {
        cout << "[Client] obstacles.txt not found — no wall overlay.\n";
    }

    // exp fill은 가로로 반복해서 채워야 하므로 wrap repeat 활성화
    exp_fill_tex->setRepeated(true);

    g_font = new sf::Font;
    // 한글 메시지를 표시할 수 있도록 Malgun Gothic(맑은 고딕)을 우선 시도. 없으면 기존 cour.ttf로 폴백.
    const char* font_paths[] = {
        "C:/Windows/Fonts/malgun.ttf",
        "malgun.ttf",
        "cour.ttf",
    };
    bool font_loaded = false;
    for (const char* p : font_paths) {
        if (g_font->loadFromFile(p)) { font_loaded = true; cout << "[Client] font loaded: " << p << "\n"; break; }
    }
    if (!font_loaded) {
        cout << "Font Loading Error!\n";
        exit(-1);
    }

    // 스킬 효과음 로드 (실패해도 경고만, 게임 계속 동작)
    auto load_sfx = [](sf::SoundBuffer& buf, sf::Sound& snd, const char* name) {
        char p1[80], p2[80], p3[80];
        std::snprintf(p1, sizeof(p1), "Resource/audio/sfx/%s", name);
        std::snprintf(p2, sizeof(p2), "../../Resource/audio/sfx/%s", name);
        std::snprintf(p3, sizeof(p3), "../../../Resource/audio/sfx/%s", name);
        for (const char* p : {p1, p2, p3}) {
            if (buf.loadFromFile(p)) {
                snd.setBuffer(buf);
                cout << "[Client] sfx loaded: " << p << "\n";
                return;
            }
        }
        cout << "[Client] sfx not found: " << name << "\n";
    };
    load_sfx(g_sfx_q_buf, g_sfx_q, "Q.wav");
    load_sfx(g_sfx_w_buf, g_sfx_w, "W.wav");
    load_sfx(g_sfx_e_buf, g_sfx_e, "E.wav");
    load_sfx(g_sfx_a_buf,       g_sfx_a,       "A.wav");          // 기본 공격
    load_sfx(g_sfx_levelup_buf, g_sfx_levelup, "LevelUp.wav");    // 레벨업
    load_sfx(g_sfx_hurt_buf,    g_sfx_hurt,    "Hurt.wav");       // 피격
    load_sfx(g_sfx_pickup_buf,  g_sfx_pickup,  "ItemPickup.wav"); // 아이템 줍기

    avatar = OBJECT{};
    avatar.set_hero(*hero_tex, hero_attack_tex);
    avatar.set_name(avatar_name.c_str());
    avatar.move(4, 4);
}

void client_finish()
{
    players.clear();
    g_effects.clear();
    delete g_font;
    delete dungeon_tiles;
    delete obstacle_tex;
    delete village_deco_tex;
    delete npcs_tex;
    delete bosses_tex;
    delete items_tex;
    delete landmarks_tex;
    delete hero_tex;
    delete hero_attack_tex;
    delete orc_tex;
    delete orb_tex;
    delete exp_frame_tex;
    delete exp_fill_tex;
    delete chat_panel_tex;
    delete blood_tex;
    delete death_tex;
    delete respawn_tex;
    delete levelup_tex;
    delete slash_tex;
    delete skill_aoe_tex;
    delete skill_line_tex;
    delete skill_heal_tex;
    delete skill_sparkle_tex;
    delete skill_icon_q_tex;
    delete skill_icon_w_tex;
    delete skill_icon_e_tex;
    delete attack_player_impact_tex;
}

void send_packet(void* packet)
{
    unsigned char* p = reinterpret_cast<unsigned char*>(packet);
    size_t sent = 0;
    socket.send(packet, p[0], sent);
}

void ProcessPacket(char* ptr)
{
    switch (ptr[1])
    {
    case S2C_LOGIN_RESULT:
    {
        S2C_LoginResult* packet = reinterpret_cast<S2C_LoginResult*>(ptr);
        if (packet->success) {
            std::cout << "Login Success!\n";
            C2S_Login p;
            p.size = sizeof(p);
            p.type = C2S_LOGIN;
            strcpy_s(p.username, avatar_name.c_str());
            send_packet(&p);
        }
        else {
            std::cout << "Login Failed!\n";
            socket.disconnect();
        }
        break;
    }
    case S2C_AVATAR_INFO:
    {
        S2C_AvatarInfo* packet = reinterpret_cast<S2C_AvatarInfo*>(ptr);
        g_myid = packet->playerId;
        avatar.m_x = packet->x;
        avatar.m_y = packet->y;

        g_left_x = packet->x - 8;
        g_top_y = packet->y - 8;
        g_my_hp = packet->hp;
        g_my_max_hp = packet->max_hp;
        g_my_mp = packet->mp;
        g_my_max_mp = packet->max_mp;
        g_my_exp = packet->exp;
        g_my_level = packet->level;
        avatar.show();
    }
    break;

    case S2C_ADD_OBJECT:
    {
        S2C_AddObject* my_packet = reinterpret_cast<S2C_AddObject*>(ptr);
        int id = my_packet->object_id;
        // 워리어/오크 모두 동일한 4방향 walk 시트 레이아웃 → set_hero 재활용
        // 다른 플레이어는 attack 시트도 전달 (공격 시 모션 재생됨)
        players[id] = OBJECT{};
        if (id >= NPC_ID_START) {
            // Stage 6.4: visual_id (1~16) → DCSS NPC 시트 column (0~15).
            // visual_id 17~20: 구역 보스 → DCSS 보스 시트 (17→col0 Slime King ... 20→col3 Cerebov)
            int col = my_packet->visual_id - 1;
            if (my_packet->visual_id >= 17 && my_packet->visual_id <= 20) {
                players[id].set_boss(*bosses_tex, my_packet->visual_id - 17);
            } else if (col >= 0 && col < 16) {
                players[id].set_npc(*npcs_tex, col);
            } else {
                players[id].set_hero(*orc_tex);  // unknown visual_id → 기존 orc
            }
        }
        else {
            players[id].set_hero(*hero_tex, hero_attack_tex);  // 다른 플레이어 = 워리어
        }
        players[id].move(my_packet->x, my_packet->y);
        players[id].set_name(my_packet->obj_name);
        if (id >= NPC_ID_START) {
            players[id].m_hp = my_packet->hp;
            players[id].m_max_hp = my_packet->max_hp;
        }
        players[id].show();
        break;
    }
    case S2C_MOVE_OBJECT:
    {
        S2C_MoveObject* my_packet = reinterpret_cast<S2C_MoveObject*>(ptr);
        int other_id = my_packet->object_id;
        if (other_id == g_myid) {
            avatar.on_move(my_packet->x, my_packet->y);
            g_left_x = my_packet->x - 8;
            g_top_y = my_packet->y - 8;
        }
        else {
            players[other_id].on_move(my_packet->x, my_packet->y);
        }
        break;
    }

    case S2C_REMOVE_OBJECT:
    {
        S2C_RemoveObject* my_packet = reinterpret_cast<S2C_RemoveObject*>(ptr);
        int other_id = my_packet->object_id;
        if (other_id == g_myid)
            avatar.hide();
        else
            players.erase(other_id);
        break;
    }
    case S2C_ATTACK_ANIM:
    {
        S2C_AttackAnim* p = reinterpret_cast<S2C_AttackAnim*>(ptr);
        int aid = p->object_id;
        int dir = p->direction;
        bool is_npc = (aid >= NPC_ID_START);
        int ax, ay;
        if (aid == g_myid) {
            avatar.on_attack(dir);
            if (g_sfx_a.getBuffer()) g_sfx_a.play();  // 내 공격 스윙음 (서버 확정 = 쿨타임 통과분만)
            ax = avatar.m_x; ay = avatar.m_y;
        } else {
            auto it = players.find(aid);
            if (it == players.end()) break;
            it->second.on_attack(dir);
            ax = it->second.m_x; ay = it->second.m_y;
        }
        if (is_npc) spawn_effect_attack_npc(ax, ay, dir);
        else        spawn_effect_attack_player(ax, ay);
        break;
    }
    case S2C_DAMAGE:
    {
        S2C_Damage* p = reinterpret_cast<S2C_Damage*>(ptr);
        spawn_effect_blood(p->target_x, p->target_y);

        // 데미지 숫자 popup: 내가 입은 건 빨강, 내가 준 건 노랑, 그 외는 회색
        sf::Color dmg_color;
        if (p->target_id == g_myid)         dmg_color = sf::Color(255,  80,  80);
        else if (p->attacker_id == g_myid)  dmg_color = sf::Color(255, 220,  80);
        else                                dmg_color = sf::Color(220, 220, 220);
        spawn_damage_popup(p->target_x, p->target_y, p->damage, dmg_color);

        if (p->target_id == g_myid) {
            if (g_sfx_hurt.getBuffer()) g_sfx_hurt.play();  // 피격음
            g_my_hp = p->new_hp;
            // 받은 데미지 크기에 비례한 화면 흔들기 (보스 AoE는 자연스럽게 큰 흔들기)
            float strength = std::min(20.0f, 2.0f + p->damage * 0.4f);
            int duration = std::min(500, 150 + p->damage * 5);
            trigger_shake(strength, duration);
        } else if (p->target_id >= NPC_ID_START) {
            auto it = players.find(p->target_id);
            if (it != players.end()) it->second.m_hp = p->new_hp;
        }

        // 메시지 창에 전투 로그 표시
        // (1) 내가 몬스터를 때림: "용사가 몬스터 A를 때려서 10의 데미지를 입혔습니다."
        if (p->attacker_id == g_myid && p->target_id >= NPC_ID_START) {
            auto it = players.find(p->target_id);
            const char* mname = (it != players.end() && it->second.name[0]) ? it->second.name : "몬스터";
            char buf[256];
            sprintf_s(buf, "용사가 %s를 때려서 %d의 데미지를 입혔습니다.", mname, p->damage);
            add_chat_line(buf);
            // 마지막 일격이면 다음 exp 증가 메시지를 위해 이름 기억
            if (p->new_hp <= 0) g_last_killed_npc_name = mname;
        }
        // (2) 몬스터가 나를 때림: "몬스터A의 공격으로 15의 데미지를 입었습니다."
        else if (p->target_id == g_myid && p->attacker_id >= NPC_ID_START) {
            auto it = players.find(p->attacker_id);
            const char* mname = (it != players.end() && it->second.name[0]) ? it->second.name : "몬스터";
            char buf[256];
            sprintf_s(buf, "%s의 공격으로 %d의 데미지를 입었습니다.", mname, p->damage);
            add_chat_line(buf);
        }
        break;
    }
    case S2C_STATUS_CHANGE:
    {
        S2C_StatusChange* p = reinterpret_cast<S2C_StatusChange*>(ptr);
        if (p->object_id == g_myid) {
            // exp 증가 + 직전에 내가 죽인 몬스터가 있으면 메시지 출력
            // (3) "몬스터 A를 무찔러서 250의 경험치를 얻었습니다."
            if (p->exp > g_my_exp && !g_last_killed_npc_name.empty()) {
                unsigned long long gained = p->exp - g_my_exp;
                char buf[256];
                sprintf_s(buf, "%s를 무찔러서 %llu의 경험치를 얻었습니다.",
                          g_last_killed_npc_name.c_str(), gained);
                add_chat_line(buf);
                g_last_killed_npc_name.clear();
            }
            g_my_hp = p->hp;
            g_my_max_hp = p->max_hp;
            g_my_exp = p->exp;
            g_my_level = p->level;
        } else if (g_party_member_ids.count(p->object_id)) {
            // 파티원 HP 바 갱신
            for (auto& m : g_party_members) {
                if (m.id == p->object_id) {
                    m.hp = p->hp;
                    m.max_hp = p->max_hp;
                    break;
                }
            }
        }
        break;
    }
    case S2C_MP_CHANGE:
    {
        S2C_MpChange* p = reinterpret_cast<S2C_MpChange*>(ptr);
        if (p->object_id == g_myid) {
            g_my_mp = p->mp;
            g_my_max_mp = p->max_mp;
        }
        break;
    }
    case S2C_DEATH:
    {
        S2C_Death* p = reinterpret_cast<S2C_Death*>(ptr);
        spawn_effect_death(p->death_x, p->death_y);
        if (p->object_id == g_myid) {
            avatar.hide();  // Phase 3b에서 플레이어 사망/리스폰 처리 추가 예정
            trigger_shake(18.0f, 600);  // 내가 죽으면 강한 임팩트
        }
        else {
            // 다른 엔티티(주로 NPC)는 화면에서 제거. 리스폰 시 S2C_ADD_OBJECT로 다시 생김
            players.erase(p->object_id);
        }
        break;
    }
    case S2C_RESPAWN:
    {
        S2C_Respawn* p = reinterpret_cast<S2C_Respawn*>(ptr);
        spawn_effect_respawn(p->respawn_x, p->respawn_y);
        if (p->object_id == g_myid) {
            avatar.move(p->respawn_x, p->respawn_y);
            // 카메라 새 위치 중심으로 이동
            g_left_x = p->respawn_x - 8;
            g_top_y  = p->respawn_y - 8;
            g_my_hp = p->hp;
            g_my_max_hp = p->max_hp;
            avatar.show();
        }
        // NPC 리스폰의 경우 서버가 S2C_ADD_OBJECT를 먼저 보냈으므로 이미 players에 있음. 이펙트만 그리면 됨.
        break;
    }
    case S2C_LEVEL_UP:
    {
        S2C_LevelUp* p = reinterpret_cast<S2C_LevelUp*>(ptr);
        if (p->object_id == g_myid) {
            spawn_effect_levelup(avatar.m_x, avatar.m_y);
            if (g_sfx_levelup.getBuffer()) g_sfx_levelup.play();  // 레벨업 효과음
            g_my_level = p->new_level;
            g_my_max_hp = p->new_max_hp;
            // HP는 곧이어 도착하는 S2C_StatusChange에서 풀로 갱신됨
        }
        else {
            auto it = players.find(p->object_id);
            if (it != players.end()) {
                spawn_effect_levelup(it->second.m_x, it->second.m_y);
            }
        }
        break;
    }
    case S2C_CHAT_MESSAGE:
    {
        S2C_ChatMessage* p = reinterpret_cast<S2C_ChatMessage*>(ptr);
        std::string sender;
        if (p->object_id == 0) {
            sender = "[System]";  // 서버 시스템 메시지 (파티 알림 등)
        } else if (p->object_id == g_myid) {
            sender = avatar_name;
        }
        else {
            auto it = players.find(p->object_id);
            if (it != players.end() && it->second.name[0] != '\0') {
                sender = it->second.name;
            }
            else {
                sender = "ID" + std::to_string(p->object_id);
            }
        }
        add_chat_line(sender + ": " + std::string(p->message));
        break;
    }
    case S2C_PARTY_INVITED:
    {
        S2C_PartyInvited* p = reinterpret_cast<S2C_PartyInvited*>(ptr);
        std::string msg = std::string(p->inviter_name) + " invited you. /accept or /reject";
        add_chat_line("[Party] " + msg);
        break;
    }
    case S2C_PARTY_UPDATE:
    {
        S2C_PartyUpdate* p = reinterpret_cast<S2C_PartyUpdate*>(ptr);
        if (p->event == 0) {
            // 파티원 입장 — 목록에 추가
            if (!g_party_member_ids.count(p->member_id)) {
                PartyMemberInfo m;
                m.id = p->member_id;
                m.name = p->member_name;
                m.hp = 0; m.max_hp = 0;
                // S2C_ADD_OBJECT로 이미 받은 HP 정보가 있으면 가져오기
                auto it = players.find(p->member_id);
                if (it != players.end()) {
                    m.hp = it->second.m_hp;
                    m.max_hp = it->second.m_max_hp;
                }
                g_party_members.push_back(m);
                g_party_member_ids.insert(p->member_id);
            }
            add_chat_line("[Party] " + std::string(p->member_name) + " joined.");
        } else if (p->event == 1) {
            // 파티원 탈퇴
            g_party_members.erase(
                std::remove_if(g_party_members.begin(), g_party_members.end(),
                    [&](const PartyMemberInfo& m){ return m.id == p->member_id; }),
                g_party_members.end());
            g_party_member_ids.erase(p->member_id);
            add_chat_line("[Party] " + std::string(p->member_name) + " left the party.");
        } else if (p->event == 2) {
            // 파티 해산
            g_party_members.clear();
            g_party_member_ids.clear();
            add_chat_line("[Party] Party disbanded.");
        }
        break;
    }
    case S2C_SKILL_EFFECT:
    {
        S2C_SkillEffect* p = reinterpret_cast<S2C_SkillEffect*>(ptr);
        if (p->skill_id == 1)      spawn_effect_skill_aoe(p->x, p->y);
        else if (p->skill_id == 2) spawn_effect_skill_line(p->x, p->y, p->direction);
        else if (p->skill_id == 3) spawn_effect_skill_heal(p->x, p->y);
        break;
    }
    // === Stage 8: 아이템 ===
    case S2C_ITEM_DROP:
    {
        S2C_ItemDrop* p = reinterpret_cast<S2C_ItemDrop*>(ptr);
        g_ground_items[p->drop_id] = GroundItemView{ p->item_id, p->x, p->y };
        break;
    }
    case S2C_ITEM_REMOVE:
    {
        S2C_ItemRemove* p = reinterpret_cast<S2C_ItemRemove*>(ptr);
        g_ground_items.erase(p->drop_id);
        break;
    }
    case S2C_INVENTORY:
    {
        S2C_Inventory* p = reinterpret_cast<S2C_Inventory*>(ptr);
        // 줍기 효과음: 인벤 총 수량이 늘었을 때만 재생.
        // 최초 스냅샷(로그인 복원)은 제외 — inv_synced 플래그로 1회차 건너뜀.
        long long prev_total = 0;
        for (const auto& it : g_inventory) prev_total += it.second;
        g_inventory.clear();
        int n = p->count; if (n > MAX_INVENTORY_SLOTS) n = MAX_INVENTORY_SLOTS;
        for (int i = 0; i < n; ++i)
            g_inventory.emplace_back(p->slots[i].item_id, p->slots[i].qty);
        long long new_total = 0;
        for (const auto& it : g_inventory) new_total += it.second;
        static bool inv_synced = false;
        if (inv_synced && new_total > prev_total && g_sfx_pickup.getBuffer())
            g_sfx_pickup.play();
        inv_synced = true;
        g_equipped_weapon_id = p->equipped_weapon_id;
        g_equipped_armor_id  = p->equipped_armor_id;
        break;
    }
    case S2C_QUEST_DIALOGUE:
    {
        S2C_QuestDialogue* p = reinterpret_cast<S2C_QuestDialogue*>(ptr);
        g_dialogue_npc   = p->npc_index;
        g_dialogue_quest = p->quest_id;
        g_dialogue_kind  = p->kind;
        g_dialogue_open  = true;
        break;
    }
    case S2C_QUEST_UPDATE:
    {
        S2C_QuestUpdate* p = reinterpret_cast<S2C_QuestUpdate*>(ptr);
        bool found = false;
        for (auto& q : g_quests) {
            if (q.id == p->quest_id) {
                q.kill_count = p->kill_count;
                q.target     = p->target_count;
                q.state      = p->state;
                found = true;
                break;
            }
        }
        if (!found) g_quests.push_back({ p->quest_id, p->kill_count, p->target_count, p->state });

        const ClientQuestMeta* m = quest_meta(p->quest_id);
        std::string title = m ? m->title : ("Quest " + std::to_string(p->quest_id));
        if (p->state == 1) {
            add_chat_line("[Quest] '" + title + "' 완료!");
        } else if (p->kill_count >= p->target_count && p->target_count > 0) {
            add_chat_line("[Quest] '" + title + "' 목표 달성 — 장로에게 보고하세요.");
        } else {
            add_chat_line("[Quest] '" + title + "' " +
                          std::to_string(p->kill_count) + "/" + std::to_string(p->target_count));
        }
        break;
    }
    }
}

void process_data(char* net_buf, size_t io_byte)
{
    char* ptr = net_buf;
    static size_t in_packet_size = 0;
    static size_t saved_packet_size = 0;
    static char packet_buffer[BUF_SIZE];

    while (0 != io_byte) {
        // size 바이트는 0~255 범위이므로 signed char로 해석하면 안 됨 (예: 209→-47).
        // unsigned char로 캐스트해서 진짜 값(209)을 얻는다. 이게 안 되면 S2C_ChatMessage(209B)
        // 같은 큰 패킷의 size가 음수로 잡혀 size_t에 거대한 값이 들어가 영원히 처리 안 됨.
        if (0 == in_packet_size) in_packet_size = static_cast<unsigned char>(ptr[0]);
        if (io_byte + saved_packet_size >= in_packet_size) {
            memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
            ProcessPacket(packet_buffer);
            ptr += in_packet_size - saved_packet_size;
            io_byte -= in_packet_size - saved_packet_size;
            in_packet_size = 0;
            saved_packet_size = 0;
        }
        else {
            memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
            saved_packet_size += io_byte;
            io_byte = 0;
        }
    }
}

// 원형 게이지: (cx, cy) 중심, radius 반지름의 원형 영역에 비율만큼 액체를 채움.
// ratio가 0이면 비어있는 배경만, 1이면 가득 찬 원, 그 사이면 호를 따라 게이지 라인이
// 그려진 모양의 ConvexShape로 채움. 사각형 모서리가 프레임 밖으로 나오는 문제 해결.
static void draw_circular_gauge(float cx, float cy, float radius,
                                float ratio, sf::Color liquid_color,
                                sf::Color empty_color)
{
    sf::CircleShape bg(radius);
    bg.setFillColor(empty_color);
    bg.setPosition(cx - radius, cy - radius);
    g_window->draw(bg);

    if (ratio <= 0.0f) return;
    if (ratio >= 1.0f) {
        sf::CircleShape full(radius);
        full.setFillColor(liquid_color);
        full.setPosition(cx - radius, cy - radius);
        g_window->draw(full);
        return;
    }

    // 게이지 레벨 라인 y: ratio=0 → cy+R (바닥), ratio=1 → cy-R (천장)
    float level_y = cy + radius * (1.0f - 2.0f * ratio);
    float dy = level_y - cy;
    float chord_half = std::sqrt(std::max(0.0f, radius * radius - dy * dy));

    // 좌측 교차점 → 원의 하단 호 → 우측 교차점 순으로 폴리곤 구성 (CCW in 스크린 좌표)
    float asin_v = std::asin(std::max(-1.0f, std::min(1.0f, dy / radius)));
    float theta_left  = PI_F - asin_v;
    float theta_right = asin_v;

    std::vector<sf::Vector2f> pts;
    pts.reserve(40);
    pts.push_back(sf::Vector2f(cx - chord_half, level_y));

    const int N = 32;
    for (int i = 1; i < N; ++i) {
        float t = (float)i / N;
        float theta = theta_left + (theta_right - theta_left) * t;
        pts.push_back(sf::Vector2f(cx + radius * std::cos(theta),
                                   cy + radius * std::sin(theta)));
    }
    pts.push_back(sf::Vector2f(cx + chord_half, level_y));

    sf::ConvexShape liquid;
    liquid.setPointCount(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) liquid.setPoint(i, pts[i]);
    liquid.setFillColor(liquid_color);
    g_window->draw(liquid);
}

// === 미니맵 (Stage 6.4) ===
// 좌상단 200x200 px, 자신 중심 ±50 타일 영역. 1타일=2px.
// 표시: 배경/마을 영역/4지역 경계선/장애물/시야내 entity/자신 방향 화살표
static void draw_minimap() {
    constexpr int   MINIMAP_SIZE = 200;
    constexpr int   MINIMAP_RANGE = 50;
    constexpr float MINIMAP_PX_PER_TILE = (float)MINIMAP_SIZE / (2.0f * MINIMAP_RANGE);
    constexpr float ox = 10.0f, oy = 10.0f;

    const int my_x = avatar.m_x;
    const int my_y = avatar.m_y;
    const int wx_min = my_x - MINIMAP_RANGE;
    const int wy_min = my_y - MINIMAP_RANGE;
    const int wx_max = my_x + MINIMAP_RANGE;
    const int wy_max = my_y + MINIMAP_RANGE;

    auto w2m = [&](int wx, int wy) -> sf::Vector2f {
        return { ox + (wx - wx_min) * MINIMAP_PX_PER_TILE,
                 oy + (wy - wy_min) * MINIMAP_PX_PER_TILE };
    };

    // 배경
    sf::RectangleShape bg(sf::Vector2f((float)MINIMAP_SIZE, (float)MINIMAP_SIZE));
    bg.setPosition(ox, oy);
    bg.setFillColor(sf::Color(20, 25, 40, 215));
    bg.setOutlineColor(sf::Color::White);
    bg.setOutlineThickness(2.0f);
    g_window->draw(bg);

    // 마을 영역 (베이지 반투명)
    {
        int vx1 = std::max(wx_min, (int)VILLAGE_X1);
        int vy1 = std::max(wy_min, (int)VILLAGE_Y1);
        int vx2 = std::min(wx_max, (int)VILLAGE_X2);
        int vy2 = std::min(wy_max, (int)VILLAGE_Y2);
        if (vx1 < vx2 && vy1 < vy2) {
            auto p1 = w2m(vx1, vy1);
            float w = (vx2 - vx1) * MINIMAP_PX_PER_TILE;
            float h = (vy2 - vy1) * MINIMAP_PX_PER_TILE;
            sf::RectangleShape vrect(sf::Vector2f(w, h));
            vrect.setPosition(p1);
            vrect.setFillColor(sf::Color(230, 215, 170, 130));
            g_window->draw(vrect);
        }
    }

    // 4지역 경계선 (x=1000, y=1000 십자)
    if (REGION_HALF >= wx_min && REGION_HALF <= wx_max) {
        float vx = ox + (REGION_HALF - wx_min) * MINIMAP_PX_PER_TILE;
        sf::RectangleShape ln(sf::Vector2f(1.0f, (float)MINIMAP_SIZE));
        ln.setPosition(vx, oy);
        ln.setFillColor(sf::Color(255, 255, 255, 80));
        g_window->draw(ln);
    }
    if (REGION_HALF >= wy_min && REGION_HALF <= wy_max) {
        float vy = oy + (REGION_HALF - wy_min) * MINIMAP_PX_PER_TILE;
        sf::RectangleShape ln(sf::Vector2f((float)MINIMAP_SIZE, 1.0f));
        ln.setPosition(ox, vy);
        ln.setFillColor(sf::Color(255, 255, 255, 80));
        g_window->draw(ln);
    }

    // 장애물 (회색 quads, VertexArray 일괄 draw)
    sf::VertexArray obstacles(sf::Quads);
    for (int wy = wy_min; wy <= wy_max; wy++) {
        for (int wx = wx_min; wx <= wx_max; wx++) {
            if (!client_is_blocked(wx, wy)) continue;
            auto p = w2m(wx, wy);
            sf::Color c(140, 140, 145);
            obstacles.append(sf::Vertex(p, c));
            obstacles.append(sf::Vertex({p.x + MINIMAP_PX_PER_TILE, p.y}, c));
            obstacles.append(sf::Vertex({p.x + MINIMAP_PX_PER_TILE, p.y + MINIMAP_PX_PER_TILE}, c));
            obstacles.append(sf::Vertex({p.x, p.y + MINIMAP_PX_PER_TILE}, c));
        }
    }
    if (obstacles.getVertexCount() > 0) g_window->draw(obstacles);

    // 시야 내 다른 entity (플레이어 = 초록, NPC = 빨강)
    for (auto& kv : players) {
        auto& p = kv.second;
        if (p.m_x < wx_min || p.m_x > wx_max || p.m_y < wy_min || p.m_y > wy_max) continue;
        auto pos = w2m(p.m_x, p.m_y);
        bool is_npc = (kv.first >= NPC_ID_START);
        bool is_party = !is_npc && g_party_member_ids.count(kv.first);
        sf::Color c = is_npc ? sf::Color(230, 70, 70)
                    : is_party ? sf::Color(80, 210, 255)   // 파티원: 청록
                    : sf::Color(70, 230, 90);               // 일반 플레이어: 녹색
        float size = MINIMAP_PX_PER_TILE * 2.0f;
        sf::RectangleShape dot(sf::Vector2f(size, size));
        dot.setPosition(pos.x - size * 0.25f, pos.y - size * 0.25f);
        dot.setFillColor(c);
        g_window->draw(dot);
    }

    // 4지역 랜드마크/보스존 (미니맵 범위 안만)
    for (const auto& d : g_landmarks) {
        if (d.x < wx_min || d.x > wx_max || d.y < wy_min || d.y > wy_max) continue;
        auto pos = w2m(d.x, d.y);
        float size = d.is_boss ? MINIMAP_PX_PER_TILE * 5.0f : MINIMAP_PX_PER_TILE * 3.5f;
        sf::Color c = d.is_boss ? sf::Color(255, 60, 60) : sf::Color(255, 220, 60);
        sf::RectangleShape dot(sf::Vector2f(size, size));
        dot.setPosition(pos.x - size / 2.0f, pos.y - size / 2.0f);
        dot.setFillColor(c);
        dot.setOutlineColor(sf::Color::Black);
        dot.setOutlineThickness(1.0f);
        g_window->draw(dot);
    }

    // 자신 (중앙 노란 삼각형, 방향 회전)
    {
        float cx = ox + MINIMAP_SIZE / 2.0f;
        float cy = oy + MINIMAP_SIZE / 2.0f;
        sf::ConvexShape arrow;
        arrow.setPointCount(3);
        arrow.setPoint(0, sf::Vector2f( 0.0f, -7.0f));  // 끝점 (Up 기준)
        arrow.setPoint(1, sf::Vector2f(-5.0f,  5.0f));
        arrow.setPoint(2, sf::Vector2f( 5.0f,  5.0f));
        arrow.setFillColor(sf::Color::Yellow);
        arrow.setOutlineColor(sf::Color::Black);
        arrow.setOutlineThickness(1.0f);
        arrow.setPosition(cx, cy);
        float rot = 0.0f;
        switch (avatar.m_direction) {
            case HERO_DIR_UP:    rot =   0.0f; break;
            case HERO_DIR_RIGHT: rot =  90.0f; break;
            case HERO_DIR_DOWN:  rot = 180.0f; break;
            case HERO_DIR_LEFT:  rot = 270.0f; break;
        }
        arrow.setRotation(rot);
        g_window->draw(arrow);
    }

    // 좌표 헤더 (미니맵 위에 작은 텍스트)
    {
        sf::Text t;
        t.setFont(*g_font);
        char buf[64];
        sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
        t.setString(buf);
        t.setCharacterSize(14);
        t.setFillColor(sf::Color::White);
        t.setOutlineColor(sf::Color::Black);
        t.setOutlineThickness(1.5f);
        t.setPosition(ox + 4, oy + MINIMAP_SIZE + 4);
        g_window->draw(t);
    }
}

// HUD 그리기 (월드 렌더 위에 오버레이). 모든 값은 g_my_* 전역에서 읽음.
// Stage 5에서 S2C_StatusChange로 hp/exp가 업데이트되면 자동으로 HUD에도 반영됨.
static void draw_hud()
{
    // 비율 계산 (max 0 가드)
    float hp_ratio_target  = (g_my_max_hp > 0) ? std::min(1.0f, (float)g_my_hp / (float)g_my_max_hp) : 0.0f;
    float mp_ratio_target = (g_my_max_mp > 0) ? std::min(1.0f, (float)g_my_mp / (float)g_my_max_mp) : 0.0f;
    // 레벨업 필요 EXP — 서버 LevelUpPlayer()와 반드시 동일해야 함: need = 100 × level^2.
    // (구버전 100<<(level-1) 지수식은 서버에서 폐기됨 + 고레벨 시프트 UB. 동기화 안 돼 바가 안 맞던 버그.)
    unsigned long long lvq = (g_my_level > 0) ? g_my_level : 1;
    unsigned long long max_exp = 100ULL * lvq * lvq;
    float exp_ratio_target = (max_exp > 0)
        ? std::min(1.0f, (float)g_my_exp / (float)max_exp) : 0.0f;

    // HP/EXP 보간: 매 프레임 target 쪽으로 lerp. 60fps 기준 약 150ms에 90% 도달.
    static float displayed_hp_ratio  = 1.0f;
    static float displayed_mp_ratio  = 1.0f;
    static float displayed_exp_ratio = 0.0f;
    const float LERP = 0.18f;
    displayed_hp_ratio  += (hp_ratio_target  - displayed_hp_ratio)  * LERP;
    displayed_mp_ratio  += (mp_ratio_target  - displayed_mp_ratio)  * LERP;
    displayed_exp_ratio += (exp_ratio_target - displayed_exp_ratio) * LERP;
    // 차이가 충분히 작으면 target에 스냅 (오래된 0.001 잔류 방지)
    if (std::abs(hp_ratio_target  - displayed_hp_ratio)  < 0.002f) displayed_hp_ratio  = hp_ratio_target;
    if (std::abs(mp_ratio_target  - displayed_mp_ratio)  < 0.002f) displayed_mp_ratio  = mp_ratio_target;
    if (std::abs(exp_ratio_target - displayed_exp_ratio) < 0.002f) displayed_exp_ratio = exp_ratio_target;
    float hp_ratio  = displayed_hp_ratio;
    float mp_ratio  = displayed_mp_ratio;
    float exp_ratio = displayed_exp_ratio;

    // 저체력(30% 미만) 경고: 빨강이 밝게 깜빡임 (sin 기반 ~0.5초 주기)
    sf::Color hp_color(180, 30, 30);
    if (hp_ratio_target < 0.30f && g_my_hp > 0) {
        static sf::Clock pulse_clock;
        float pulse_ms = (float)pulse_clock.getElapsedTime().asMilliseconds();
        float pulse = 0.5f + 0.5f * std::sin(pulse_ms * 0.012f);
        sf::Uint8 r  = (sf::Uint8)(180 + (255 - 180) * pulse);
        sf::Uint8 gb = (sf::Uint8)(30  + (90  - 30 ) * pulse);
        hp_color = sf::Color(r, gb, gb);
    }

    // ---- 로그 패널 (좌하단, HP orb 위, 최대 10줄) ----
    // HP orb: y = WINDOW_HEIGHT - 128 - 50 = 846. 레벨 배지: orb_y - 18 ≈ 828.
    constexpr float LOG_LINE_H  = 15.0f;
    constexpr int   LOG_LINES   = 10;
    constexpr float LOG_X       = 10.0f;
    // orb_y=846, 레벨배지 top≈812. 입력박스 bottom이 812보다 작아야 가려지지 않음.
    // LOG_BOTTOM=776 → 입력박스 778~800, 레벨배지 812~ 로 12px 여유.
    const float     LOG_BOTTOM  = (float)WINDOW_HEIGHT - 128.0f - 50.0f - 70.0f; // 776
    const float     LOG_TOP     = LOG_BOTTOM - LOG_LINE_H * LOG_LINES;            // 626
    {
        sf::Text txt;
        txt.setFont(*g_font);
        txt.setCharacterSize(13);
        txt.setOutlineColor(sf::Color::Black);
        txt.setOutlineThickness(1.0f);
        // 최근 LOG_LINES 줄만 표시 (오래된 것은 위쪽)
        int total = (int)g_chat_log.size();
        int start = std::max(0, total - LOG_LINES);
        for (int i = start; i < total; ++i) {
            int row = i - start;
            // 시스템 메시지(접두 "[") 흰색, 일반 채팅 황금색
            bool is_sys = !g_chat_log[i].empty() && g_chat_log[i][0] == '[';
            txt.setFillColor(is_sys ? sf::Color(200, 200, 200) : sf::Color(245, 235, 200));
            txt.setString(sf::String::fromUtf8(g_chat_log[i].begin(), g_chat_log[i].end()));
            txt.setPosition(LOG_X, LOG_TOP + row * LOG_LINE_H);
            g_window->draw(txt);
        }
    }

    // 채팅 입력 박스 (Enter 모드일 때만, 로그 바로 아래)
    if (g_chat_input_mode) {
        constexpr float input_w = 280.0f;
        const float input_y = LOG_BOTTOM + 2.0f;
        sf::RectangleShape box(sf::Vector2f(input_w, 22.0f));
        box.setFillColor(sf::Color(10, 8, 20, 230));
        box.setOutlineColor(sf::Color(230, 190, 90));
        box.setOutlineThickness(2.0f);
        box.setPosition(LOG_X, input_y);
        g_window->draw(box);

        sf::Text inp;
        inp.setFont(*g_font);
        inp.setCharacterSize(14);
        inp.setFillColor(sf::Color::White);
        static sf::Clock cursor_clock;
        bool show_cursor = (cursor_clock.getElapsedTime().asMilliseconds() / 500) % 2 == 0;
        inp.setString("> " + g_chat_buffer + (show_cursor ? "_" : " "));
        inp.setPosition(LOG_X + 6.0f, input_y + 2.0f);
        g_window->draw(inp);
    }

    // ---- 파티 HP 바 패널 (미니맵 오른쪽, 파티원이 있을 때만) ----
    // 미니맵: x=10, y=10, 200x200 → 파티패널 x=218, y=10
    if (!g_party_members.empty()) {
        constexpr float PARTY_W  = 200.0f;
        const float PARTY_X = 218.0f;
        const float PARTY_Y = 10.0f;
        constexpr float ROW_H = 22.0f;
        float total_h = g_party_members.size() * ROW_H + 6.0f;

        sf::RectangleShape pbg(sf::Vector2f(PARTY_W, total_h));
        pbg.setFillColor(sf::Color(10, 8, 22, 200));
        pbg.setOutlineColor(sf::Color(80, 160, 255, 180));
        pbg.setOutlineThickness(1.0f);
        pbg.setPosition(PARTY_X, PARTY_Y);
        g_window->draw(pbg);

        for (size_t i = 0; i < g_party_members.size(); ++i) {
            const auto& m = g_party_members[i];
            float row_y = PARTY_Y + 3.0f + i * ROW_H;

            sf::Text ntxt;
            ntxt.setFont(*g_font);
            ntxt.setCharacterSize(11);
            ntxt.setFillColor(sf::Color(160, 210, 255));
            ntxt.setString(m.name.size() > 8 ? m.name.substr(0, 8) : m.name);
            ntxt.setPosition(PARTY_X + 4.0f, row_y + 3.0f);
            g_window->draw(ntxt);

            float bar_x = PARTY_X + 72.0f;
            float bar_w = PARTY_W - 76.0f;
            float bar_h = 14.0f;

            sf::RectangleShape bg_bar(sf::Vector2f(bar_w, bar_h));
            bg_bar.setFillColor(sf::Color(30, 20, 20, 200));
            bg_bar.setPosition(bar_x, row_y + 4.0f);
            g_window->draw(bg_bar);

            if (m.max_hp > 0) {
                float ratio = std::max(0.0f, std::min(1.0f, (float)m.hp / (float)m.max_hp));
                sf::Color hp_c = (ratio > 0.5f) ? sf::Color(50, 200, 50)
                               : (ratio > 0.25f) ? sf::Color(200, 150, 50)
                               : sf::Color(200, 50, 50);
                sf::RectangleShape hp_bar(sf::Vector2f(bar_w * ratio, bar_h));
                hp_bar.setFillColor(hp_c);
                hp_bar.setPosition(bar_x, row_y + 4.0f);
                g_window->draw(hp_bar);
            }
        }
    }

    // ---- HP/MP 구슬 (좌하단/우하단, 각 256x256 → 128x128) ----
    // 게이지를 원형(ConvexShape)으로 그려서 프레임의 원형 inner area에 정확히 맞춤
    constexpr float ORB_SIZE     = 128.0f;
    constexpr float ORB_INNER_R  = 44.0f;  // 게이지의 반지름 — 프레임 안쪽 원과 일치
    float hp_x = 10.0f;
    float mp_x = WINDOW_WIDTH - ORB_SIZE - 10.0f;
    float orb_y = WINDOW_HEIGHT - ORB_SIZE - 50.0f;  // EXP 바 위에 50px 여유

    sf::Color empty_bg(20, 10, 10, 240);  // 매우 어두운 적자색 (액체 비어있을 때 보임)

    // HP 게이지 (빨강) — 좌측 구슬. 저체력 시 hp_color가 sin 깜빡임.
    draw_circular_gauge(hp_x + ORB_SIZE * 0.5f, orb_y + ORB_SIZE * 0.5f, ORB_INNER_R,
                        hp_ratio, hp_color, empty_bg);
    hud_sprite.setTexture(*orb_tex, true);
    hud_sprite.setTextureRect(sf::IntRect(0, 0, 256, 256));  // HP는 좌측
    hud_sprite.setScale(0.5f, 0.5f);
    hud_sprite.setPosition(hp_x, orb_y);
    g_window->draw(hud_sprite);

    // MP 게이지 (파랑) — 우측 구슬
    sf::Color mp_empty(10, 10, 20, 240);
    draw_circular_gauge(mp_x + ORB_SIZE * 0.5f, orb_y + ORB_SIZE * 0.5f, ORB_INNER_R,
                        mp_ratio, sf::Color(30, 60, 200), mp_empty);
    hud_sprite.setTexture(*orb_tex, true);
    hud_sprite.setTextureRect(sf::IntRect(256, 0, 256, 256));  // MP는 우측
    hud_sprite.setScale(0.5f, 0.5f);
    hud_sprite.setPosition(mp_x, orb_y);
    g_window->draw(hud_sprite);

    // ---- EXP 바 (하단 중앙, 1024x48 → 720x32) ----
    {
        constexpr float BAR_W = 720.0f, BAR_H = 32.0f;
        constexpr float INNER_PAD_X = 30.0f;  // 양 끝 해골 장식 영역 (스케일 후)
        constexpr float INNER_PAD_Y = 6.0f;
        float bar_x = (WINDOW_WIDTH - BAR_W) * 0.5f;
        float bar_y = WINDOW_HEIGHT - BAR_H - 8.0f;

        // 프레임 먼저 (어두운 배경이 바닥에 깔림)
        hud_sprite.setTexture(*exp_frame_tex, true);
        hud_sprite.setTextureRect(sf::IntRect(0, 0, 1024, 48));
        hud_sprite.setScale(BAR_W / 1024.0f, BAR_H / 48.0f);
        hud_sprite.setPosition(bar_x, bar_y);
        g_window->draw(hud_sprite);

        // 채움 (프레임 위에, 안쪽 영역만, 비율만큼)
        // sf::Sprite + setRepeated(true) + 큰 IntRect 조합이 일부 GPU/드라이버에서
        // 그려지지 않는 문제가 있어 단순한 RectangleShape(플랫 컬러)로 교체.
        if (exp_ratio > 0.0f) {
            float fill_pixel_w = (BAR_W - INNER_PAD_X * 2.0f) * exp_ratio;
            float fill_pixel_h = BAR_H - INNER_PAD_Y * 2.0f;

            // 본체: 황금색 그라데이션 느낌을 단색으로 근사
            sf::RectangleShape fill(sf::Vector2f(fill_pixel_w, fill_pixel_h));
            fill.setFillColor(sf::Color(240, 200, 60));
            fill.setPosition(bar_x + INNER_PAD_X, bar_y + INNER_PAD_Y);
            g_window->draw(fill);

            // 상단 하이라이트(밝은 띠)로 입체감 부여
            float hi_h = std::max(1.0f, fill_pixel_h * 0.35f);
            sf::RectangleShape hi(sf::Vector2f(fill_pixel_w, hi_h));
            hi.setFillColor(sf::Color(255, 235, 140, 200));
            hi.setPosition(bar_x + INNER_PAD_X, bar_y + INNER_PAD_Y);
            g_window->draw(hi);
        }

        // 레벨 + EXP 텍스트 (EXP 바 가운데에 오버레이)
        {
            sf::Text txt;
            txt.setFont(*g_font);
            char buf[80];
            sprintf_s(buf, "Lv %u    EXP  %llu / %llu",
                (unsigned)g_my_level,
                (unsigned long long)g_my_exp,
                (unsigned long long)max_exp);
            txt.setString(buf);
            txt.setCharacterSize(16);
            txt.setFillColor(sf::Color::White);
            txt.setOutlineColor(sf::Color::Black);
            txt.setOutlineThickness(2.0f);
            txt.setStyle(sf::Text::Bold);
            sf::FloatRect b = txt.getLocalBounds();
            txt.setOrigin(b.left + b.width / 2.0f, b.top + b.height / 2.0f);
            txt.setPosition(bar_x + BAR_W / 2.0f, bar_y + BAR_H / 2.0f);
            g_window->draw(txt);
        }
    }

    // ---- 큰 레벨 배지 (HP orb 위에) ----
    {
        sf::Text lv;
        lv.setFont(*g_font);
        char buf[32];
        sprintf_s(buf, "Lv %u", (unsigned)g_my_level);
        lv.setString(buf);
        lv.setCharacterSize(28);
        lv.setFillColor(sf::Color(255, 230, 90));
        lv.setOutlineColor(sf::Color::Black);
        lv.setOutlineThickness(3.0f);
        lv.setStyle(sf::Text::Bold);
        sf::FloatRect b = lv.getLocalBounds();
        lv.setOrigin(b.left + b.width / 2.0f, b.top + b.height / 2.0f);
        float center_hp = 10.0f + ORB_SIZE * 0.5f;
        lv.setPosition(center_hp, orb_y - 18.0f);
        g_window->draw(lv);
    }

    // ---- 스킬 슬롯 HUD (하단 중앙 EXP 바 위쪽) ----
    // Q/W/E 3칸. 쿨타임 중이면 어두운 오버레이 + 남은 초 표시
    {
        constexpr float SLOT_SIZE = 48.0f;
        constexpr float SLOT_GAP  = 8.0f;
        float total_w = SLOT_SIZE * 3 + SLOT_GAP * 2;
        float slot_x0 = (WINDOW_WIDTH - total_w) * 0.5f;
        float slot_y  = WINDOW_HEIGHT - 32.0f - SLOT_SIZE - 12.0f;  // EXP 바 바로 위

        const char* labels[3]   = { "Q", "W", "E" };
        sf::Clock*  clocks[3]   = { &g_skill1_clock, &g_skill2_clock, &g_skill3_clock };
        bool*       used[3]     = { &g_skill1_used,  &g_skill2_used,  &g_skill3_used };
        int         cds[3]      = { SKILL1_CD_MS, SKILL2_CD_MS, SKILL3_CD_MS };
        sf::Color   slot_colors[3] = {
            sf::Color(200, 60, 60),    // Q AoE — 빨강
            sf::Color(60, 120, 220),   // W Line — 파랑
            sf::Color(60, 180, 80)     // E Heal — 초록
        };
        sf::Texture* icon_texs[3] = { skill_icon_q_tex, skill_icon_w_tex, skill_icon_e_tex };

        sf::Text sk_txt;
        sk_txt.setFont(*g_font);
        sk_txt.setCharacterSize(16);
        sk_txt.setStyle(sf::Text::Bold);
        sk_txt.setOutlineColor(sf::Color::Black);
        sk_txt.setOutlineThickness(2.0f);

        sf::Sprite icon_spr;

        for (int i = 0; i < 3; ++i) {
            float sx = slot_x0 + i * (SLOT_SIZE + SLOT_GAP);

            // 배경 슬롯
            sf::RectangleShape slot(sf::Vector2f(SLOT_SIZE, SLOT_SIZE));
            slot.setFillColor(sf::Color(30, 30, 30, 200));
            slot.setOutlineColor(slot_colors[i]);
            slot.setOutlineThickness(2.0f);
            slot.setPosition(sx, slot_y);
            g_window->draw(slot);

            // 스킬 아이콘 (32x32 원본 → 40x40으로 중앙 배치)
            icon_spr.setTexture(*icon_texs[i], true);
            icon_spr.setTextureRect(sf::IntRect(0, 0, 32, 32));
            icon_spr.setScale(40.0f / 32.0f, 40.0f / 32.0f);
            icon_spr.setPosition(sx + 4.0f, slot_y + 4.0f);
            g_window->draw(icon_spr);

            long long elapsed = clocks[i]->getElapsedTime().asMilliseconds();
            bool on_cd = *used[i] && (elapsed < cds[i]);

            // 쿨타임 오버레이
            if (on_cd) {
                float cd_ratio = 1.0f - (float)elapsed / cds[i];
                sf::RectangleShape overlay(sf::Vector2f(SLOT_SIZE, SLOT_SIZE * cd_ratio));
                overlay.setFillColor(sf::Color(0, 0, 0, 160));
                overlay.setPosition(sx, slot_y);
                g_window->draw(overlay);

                // 남은 초
                char rem[8];
                int rem_s = (cds[i] - (int)elapsed + 999) / 1000;
                sprintf_s(rem, "%ds", rem_s);
                sk_txt.setString(rem);
                sk_txt.setFillColor(sf::Color(220, 220, 220));
            } else {
                sk_txt.setFillColor(sf::Color::White);
            }

            // 키 레이블
            sk_txt.setString(labels[i]);
            sf::FloatRect b2 = sk_txt.getLocalBounds();
            sk_txt.setOrigin(b2.left + b2.width / 2.0f, b2.top + b2.height / 2.0f);
            sk_txt.setPosition(sx + SLOT_SIZE / 2.0f, slot_y + SLOT_SIZE - 14.0f);
            g_window->draw(sk_txt);

            // 쿨타임 중이면 숫자도 중앙에 표시
            if (on_cd) {
                int rem_s = (cds[i] - (int)elapsed + 999) / 1000;
                char rem[8]; sprintf_s(rem, "%d", rem_s);
                sk_txt.setString(rem);
                sk_txt.setCharacterSize(22);
                sf::FloatRect b3 = sk_txt.getLocalBounds();
                sk_txt.setOrigin(b3.left + b3.width / 2.0f, b3.top + b3.height / 2.0f);
                sk_txt.setPosition(sx + SLOT_SIZE / 2.0f, slot_y + SLOT_SIZE / 2.0f - 6.0f);
                sk_txt.setFillColor(sf::Color(255, 200, 100));
                g_window->draw(sk_txt);
                sk_txt.setCharacterSize(16);  // 복원
            }
        }
    }

    // ---- 조작 힌트 (우상단 고정) ----
    constexpr float KH_X    = (float)WINDOW_WIDTH - 230.0f;
    constexpr float KH_Y    = 12.0f;
    constexpr float KH_LINE = 16.0f;
    {
        const char* hints[] = {
            "[Arrow] Move",
            "[A] Attack",
            "[Q] Fire  [W] Lightning  [E] Heal",
            "[I] Inventory  [G] Pickup",
            "[J] Quest  [T] Talk",
            "[Enter] Chat  [Esc] Quit",
        };
        sf::Text t; t.setFont(*g_font);
        t.setCharacterSize(13);
        t.setFillColor(sf::Color(200, 200, 200));
        t.setOutlineColor(sf::Color::Black);
        t.setOutlineThickness(1.0f);
        for (int hi = 0; hi < 6; ++hi) {
            t.setString(hints[hi]);
            t.setPosition(KH_X, KH_Y + hi * KH_LINE);
            g_window->draw(t);
        }
    }

    // ---- 퀘스트 진행상황 (키 바인딩 힌트 바로 아래, 항상 표시) ----
    {
        // 한글 포함 문자열을 안전하게 출력하는 인라인 헬퍼
        auto qtext = [&](const std::string& s, float x, float y, sf::Color c) {
            sf::Text qt; qt.setFont(*g_font);
            qt.setCharacterSize(12);
            qt.setFillColor(c);
            qt.setOutlineColor(sf::Color::Black);
            qt.setOutlineThickness(1.0f);
            qt.setString(sf::String::fromUtf8(s.begin(), s.end()));
            qt.setPosition(x, y);
            g_window->draw(qt);
        };

        float qy = KH_Y + 6 * KH_LINE + 8.0f;

        if (g_quests.empty()) {
            qtext("[ No quests ]", KH_X, qy, sf::Color(150, 150, 150));
        } else {
            qtext("-- Quests --", KH_X, qy, sf::Color(130, 170, 200));
            qy += 14.0f;

            for (const auto& q : g_quests) {
                const ClientQuestMeta* m = quest_meta(q.id);
                std::string title = m ? std::string(m->title) : ("Quest " + std::to_string(q.id));
                if (q.state == 1) {
                    qtext(title + "  (done)", KH_X, qy, sf::Color(120, 120, 120));
                } else if (q.kill_count >= q.target) {
                    qtext(title + "  [!]", KH_X, qy, sf::Color(255, 220, 80));
                } else {
                    char prog[16]; sprintf_s(prog, " %d/%d", q.kill_count, q.target);
                    qtext(title + prog, KH_X, qy, sf::Color(200, 230, 255));
                }
                qy += 14.0f;
                if (qy > KH_Y + 6 * KH_LINE + 90.0f) break;
            }
        }
    }

    // ---- Stage 8: 인벤토리 패널 (I 토글) ----
    if (g_inv_open) {
        constexpr float PW = 400.0f, PH = 392.0f;
        float panel_x = (WINDOW_WIDTH - PW) * 0.5f;
        float panel_y = (WINDOW_HEIGHT - PH) * 0.5f;
        sf::RectangleShape bg(sf::Vector2f(PW, PH));
        bg.setFillColor(sf::Color(15, 12, 28, 238));
        bg.setOutlineColor(sf::Color(230, 190, 90));
        bg.setOutlineThickness(2.0f);
        bg.setPosition(panel_x, panel_y);
        g_window->draw(bg);

        auto panel_text = [&](const std::string& s, float x, float y, unsigned sz, sf::Color c) {
            sf::Text t; t.setFont(*g_font);
            t.setString(sf::String::fromUtf8(s.begin(), s.end()));
            t.setCharacterSize(sz); t.setFillColor(c);
            t.setOutlineColor(sf::Color::Black); t.setOutlineThickness(1.0f);
            t.setPosition(x, y); g_window->draw(t);
        };

        panel_text("Inventory  [I] close", panel_x + 14, panel_y + 10, 18, sf::Color(255, 230, 150));
        const ClientItemMeta* wm = item_meta(g_equipped_weapon_id);
        const ClientItemMeta* am = item_meta(g_equipped_armor_id);
        panel_text(std::string("Weapon: ") + (wm ? wm->name : "(none)"), panel_x + 14, panel_y + 40, 14, sf::Color(180, 210, 255));
        panel_text(std::string("Armor : ") + (am ? am->name : "(none)"), panel_x + 14, panel_y + 60, 14, sf::Color(220, 200, 150));

        constexpr int   COLS = 5;
        constexpr float CELL = 66.0f, CELL_GAP = 6.0f;
        float grid_x = panel_x + 14;
        float grid_y = panel_y + 92;
        for (int i = 0; i < MAX_INVENTORY_SLOTS; ++i) {
            int col = i % COLS, row = i / COLS;
            float cx = grid_x + col * (CELL + CELL_GAP);
            float cy = grid_y + row * (CELL + CELL_GAP);
            bool is_cursor = (i == g_inv_cursor);
            sf::RectangleShape cell(sf::Vector2f(CELL, CELL));
            cell.setFillColor(is_cursor ? sf::Color(58, 52, 82, 235) : sf::Color(30, 28, 45, 220));
            cell.setOutlineColor(is_cursor ? sf::Color(255, 220, 90) : sf::Color(90, 90, 110));
            cell.setOutlineThickness(is_cursor ? 3.0f : 1.0f);
            cell.setPosition(cx, cy);
            g_window->draw(cell);

            if (i < 10) {  // 앞 10칸만 숫자키 매핑 (1..9, 0)
                int keynum = (i == 9) ? 0 : (i + 1);
                char kb[4]; sprintf_s(kb, "%d", keynum);
                panel_text(kb, cx + 4, cy + 2, 12, sf::Color(160, 160, 160));
            }

            if (i < (int)g_inventory.size()) {
                int item_id = g_inventory[i].first;
                int qty = g_inventory[i].second;
                const ClientItemMeta* m = item_meta(item_id);
                draw_item_sprite(item_id, cx + CELL * 0.5f, cy + 28, 40.0f);

                std::string nm = m ? m->name : "item";
                if (nm.size() > 10) nm = nm.substr(0, 10);
                panel_text(nm, cx + 3, cy + CELL - 28, 10, sf::Color(235, 235, 235));
                if (qty > 1) { char qb[8]; sprintf_s(qb, "x%d", qty); panel_text(qb, cx + CELL - 28, cy + CELL - 15, 12, sf::Color(255, 240, 160)); }
            }
        }

        panel_text("Arrows: move cursor   F/Enter: use/equip   D: drop", panel_x + 14, panel_y + PH - 42, 12, sf::Color(255, 220, 130));
        panel_text("1-0: quick slot   R/A: unequip wpn/arm   G: pick up", panel_x + 14, panel_y + PH - 24, 12, sf::Color(200, 200, 200));
    }

    // 공용 텍스트 헬퍼 (퀘스트 UI)
    auto hud_text = [&](const std::string& s, float x, float y, unsigned sz, sf::Color c) {
        sf::Text t; t.setFont(*g_font);
        t.setString(sf::String::fromUtf8(s.begin(), s.end()));
        t.setCharacterSize(sz); t.setFillColor(c);
        t.setOutlineColor(sf::Color::Black); t.setOutlineThickness(1.0f);
        t.setPosition(x, y); g_window->draw(t);
    };

    // Stage 9: 퀘스트 로그 패널 (J 토글) — 화면 우측 상단
    if (g_quest_log_open) {
        constexpr float PW = 320.0f, PH = 230.0f;
        float px = WINDOW_WIDTH - PW - 16.0f;
        float py = 80.0f;
        sf::RectangleShape bg(sf::Vector2f(PW, PH));
        bg.setFillColor(sf::Color(15, 12, 28, 232));
        bg.setOutlineColor(sf::Color(130, 200, 255));
        bg.setOutlineThickness(2.0f);
        bg.setPosition(px, py);
        g_window->draw(bg);

        hud_text("퀘스트  [J] 닫기", px + 14, py + 10, 18, sf::Color(150, 220, 255));
        float ty = py + 42;
        if (g_quests.empty()) {
            hud_text("진행 중인 퀘스트가 없습니다.", px + 14, ty, 14, sf::Color(190, 190, 190));
            hud_text("마을 북서쪽 장로에게 T로 말을 거세요.", px + 14, ty + 22, 13, sf::Color(160, 160, 160));
        } else {
            for (const auto& q : g_quests) {
                const ClientQuestMeta* m = quest_meta(q.id);
                std::string title = m ? m->title : ("Quest " + std::to_string(q.id));
                if (q.state == 1) {
                    hud_text(title + "  (완료)", px + 14, ty, 14, sf::Color(140, 140, 140));
                } else if (q.kill_count >= q.target) {
                    hud_text(title + "  [보고 가능]", px + 14, ty, 14, sf::Color(255, 220, 120));
                } else {
                    char prog[16]; sprintf_s(prog, "%d/%d", q.kill_count, q.target);
                    hud_text(title + "  " + prog, px + 14, ty, 14, sf::Color(220, 235, 255));
                }
                ty += 24;
                if (ty > py + PH - 20) break;
            }
        }
    }

    // Stage 9: 퀘스트 대화창 (장로와 대화 시) — 화면 하단 중앙
    if (g_dialogue_open) {
        constexpr float PW = 560.0f, PH = 150.0f;
        float px = (WINDOW_WIDTH - PW) * 0.5f;
        float py = WINDOW_HEIGHT - PH - 40.0f;
        sf::RectangleShape bg(sf::Vector2f(PW, PH));
        bg.setFillColor(sf::Color(10, 10, 20, 240));
        bg.setOutlineColor(sf::Color(230, 200, 110));
        bg.setOutlineThickness(2.0f);
        bg.setPosition(px, py);
        g_window->draw(bg);

        hud_text("장로 (Elder)", px + 16, py + 10, 16, sf::Color(255, 225, 150));

        const ClientQuestMeta* m = quest_meta(g_dialogue_quest);
        std::string title = m ? m->title : ("Quest " + std::to_string(g_dialogue_quest));
        std::string desc  = m ? m->desc : "";

        if (g_dialogue_kind == 0) {  // Offer
            hud_text("[새 퀘스트] " + title, px + 16, py + 40, 16, sf::Color(180, 230, 255));
            hud_text(desc, px + 16, py + 66, 13, sf::Color(220, 220, 220));
            hud_text("[Y] 수락     [Esc] 닫기", px + 16, py + PH - 28, 14, sf::Color(160, 255, 160));
        } else if (g_dialogue_kind == 2) {  // ReadyTurnIn
            hud_text(title + " — 목표 달성!", px + 16, py + 40, 16, sf::Color(255, 220, 120));
            hud_text("수고했네. 보상을 받게나.", px + 16, py + 66, 13, sf::Color(220, 220, 220));
            hud_text("[Y] 보상 받기     [Esc] 닫기", px + 16, py + PH - 28, 14, sf::Color(255, 220, 120));
        } else if (g_dialogue_kind == 1) {  // InProgress
            int kc = 0, tg = 0;
            for (const auto& q : g_quests) if (q.id == g_dialogue_quest) { kc = q.kill_count; tg = q.target; break; }
            char prog[24]; sprintf_s(prog, "진행 중: %d/%d", kc, tg);
            hud_text(title, px + 16, py + 40, 16, sf::Color(200, 220, 255));
            hud_text(prog, px + 16, py + 66, 14, sf::Color(220, 235, 255));
            hud_text("[Esc] 닫기", px + 16, py + PH - 28, 14, sf::Color(200, 200, 200));
        } else {  // None
            hud_text("지금은 줄 퀘스트가 없네. 다음에 또 오게.", px + 16, py + 44, 14, sf::Color(210, 210, 210));
            hud_text("[Esc] 닫기", px + 16, py + PH - 28, 14, sf::Color(200, 200, 200));
        }
    }
}

void client_main()
{
    char net_buf[BUF_SIZE];
    size_t	received;

    auto recv_result = socket.receive(net_buf, BUF_SIZE, received);
    if (recv_result == sf::Socket::Error || recv_result == sf::Socket::Disconnected) exit(-1);

    if (recv_result != sf::Socket::NotReady)
        if (received > 0) process_data(net_buf, received);

    // 화면 흔들기 적용: 게임 월드/캐릭터/이펙트는 흔들리는 view로, HUD는 기본 view로
    sf::View default_view = g_window->getDefaultView();
    sf::View shake_view = default_view;
    sf::Vector2f shake_off = get_shake_offset();
    shake_view.move(shake_off.x, shake_off.y);
    g_window->setView(shake_view);

    for (int i = 0; i < SCREEN_WIDTH; ++i) {
        for (int j = 0; j < SCREEN_HEIGHT; ++j)
        {
            int tile_x = i + g_left_x;
            int tile_y = j + g_top_y;

            if ((tile_x < 0) || (tile_y < 0) || (tile_x >= WORLD_WIDTH) || (tile_y >= WORLD_HEIGHT)) continue;

            // 지역 결정: 2000x2000을 4분할 → 행 인덱스(0~3) = 타일시트의 row
            //   NW(x<1000,y<1000)=0 균열석 / SW(x<1000,y>=1000)=1 혈흔
            //   NE(x>=1000,y<1000)=2 벽돌  / SE(x>=1000,y>=1000)=3 룬
            int region_row, variant_col;
            // 마을 영역 안: 단일 sandstone 타일로 통일 (광장 일체감)
            if (tile_x >= VILLAGE_X1 && tile_x < VILLAGE_X2 &&
                tile_y >= VILLAGE_Y1 && tile_y < VILLAGE_Y2) {
                region_row = 2;   // row 2 = NE 사암 패밀리 (밝은 베이지)
                variant_col = 2;  // col 2 = sandstone_floor_0 (단일 variant 고정)
            } else {
                region_row = (tile_x >= REGION_HALF ? 2 : 0) + (tile_y >= REGION_HALF ? 1 : 0);
                // variant: 같은 (wx,wy)는 항상 같은 variant (좌표 해시) → 패턴이 자연스럽게 섞임
                unsigned int h = (unsigned int)(tile_x * 73856093) ^ (unsigned int)(tile_y * 19349663);
                variant_col = h & 0x3;
            }

            tile_sprite.setTextureRect(sf::IntRect(
                variant_col * TILE_SRC_SIZE, region_row * TILE_SRC_SIZE,
                TILE_SRC_SIZE, TILE_SRC_SIZE));
            tile_sprite.setPosition((float)(TILE_WIDTH * i), (float)(TILE_WIDTH * j));
            g_window->draw(tile_sprite);

            // 마을 바닥 오버레이 (Aetheria Village 영역 안만)
            if (tile_x >= VILLAGE_X1 && tile_x < VILLAGE_X2 &&
                tile_y >= VILLAGE_Y1 && tile_y < VILLAGE_Y2) {
                g_village_floor.setPosition((float)(TILE_WIDTH * i), (float)(TILE_WIDTH * j));
                g_window->draw(g_village_floor);
            }

            // 명시적 장애물 (obstacles.txt rect) — 지역/마을 외벽에 따라 column 선택
            if (client_is_obstacle_bit(tile_x, tile_y)) {
                int obs_col;
                bool in_village = (tile_x >= VILLAGE_X1 && tile_x < VILLAGE_X2 &&
                                   tile_y >= VILLAGE_Y1 && tile_y < VILLAGE_Y2);
                if (in_village) {
                    obs_col = 4;  // brick (마을 외벽)
                } else {
                    // 0=NW설원(crystal) / 1=SW초원(tree_1_yellow) / 2=NE사막(sandstone) / 3=SE숲(tree)
                    obs_col = (tile_x >= REGION_HALF ? 2 : 0) + (tile_y >= REGION_HALF ? 1 : 0);
                }
                obstacle_sprite.setTextureRect(sf::IntRect(obs_col * 32, 0, 32, 32));
                obstacle_sprite.setPosition((float)(TILE_WIDTH * i), (float)(TILE_WIDTH * j));
                g_window->draw(obstacle_sprite);
            }
            // Stage 6.4: 절차적 장식 (장애물로 취급 — 서버/클라 동일 해시)
            else if (client_is_procedural_deco(tile_x, tile_y)) {
                // region 0=NW(crystal) / 1=SW(flowers) / 2=NE(column) / 3=SE(tree)
                int rr = (tile_x >= REGION_HALF ? 2 : 0) + (tile_y >= REGION_HALF ? 1 : 0);
                static const int kRegionDecoCol[4] = { 8, 10, 9, 11 };
                int deco_col = kRegionDecoCol[rr];
                landmark_sprite.setTextureRect(sf::IntRect(deco_col * 32, 0, 32, 32));
                landmark_sprite.setPosition((float)(TILE_WIDTH * i), (float)(TILE_WIDTH * j));
                g_window->draw(landmark_sprite);
            }
        }
    }

    // === 마을 장식 (Stage 6.4 DCSS sprite) — 시야 안일 때만 그림 ===
    auto in_view_tile = [](int wx, int wy) {
        return wx >= g_left_x && wx < g_left_x + SCREEN_WIDTH
            && wy >= g_top_y  && wy < g_top_y  + SCREEN_HEIGHT;
    };
    // 헬퍼: 마을 데코 sprite를 (tile_x, tile_y) 위치에 col 인덱스로 그림
    auto draw_village_sprite = [&](int wx, int wy, int col) {
        village_deco_sprite.setTextureRect(sf::IntRect(col * 32, 0, 32, 32));
        float px = (wx - g_left_x) * (float)TILE_WIDTH;
        float py = (wy - g_top_y)  * (float)TILE_WIDTH;
        village_deco_sprite.setPosition(px, py);
        g_window->draw(village_deco_sprite);
    };
    // 4 모서리 동상
    for (const auto& st : g_village_statues) {
        if (!in_view_tile(st.x, st.y)) continue;
        draw_village_sprite(st.x, st.y, 1);  // col 1 = statue_angel
    }
    // 중앙 분수
    if (in_view_tile(FOUNTAIN_X, FOUNTAIN_Y)) {
        draw_village_sprite(FOUNTAIN_X, FOUNTAIN_Y, 0);  // col 0 = sparkling_fountain
    }
    // 입구 4방향 게이트 sprite (외벽 입구 양쪽 1칸)
    if (in_view_tile(994, VILLAGE_Y1)) draw_village_sprite(994, VILLAGE_Y1, 6);  // 북 좌
    if (in_view_tile(1005, VILLAGE_Y1)) draw_village_sprite(1005, VILLAGE_Y1, 7); // 북 우
    if (in_view_tile(994, VILLAGE_Y2-1)) draw_village_sprite(994, VILLAGE_Y2-1, 6);
    if (in_view_tile(1005, VILLAGE_Y2-1)) draw_village_sprite(1005, VILLAGE_Y2-1, 7);
    if (in_view_tile(VILLAGE_X1, 994)) draw_village_sprite(VILLAGE_X1, 994, 6);
    if (in_view_tile(VILLAGE_X1, 1005)) draw_village_sprite(VILLAGE_X1, 1005, 7);
    if (in_view_tile(VILLAGE_X2-1, 994)) draw_village_sprite(VILLAGE_X2-1, 994, 6);
    if (in_view_tile(VILLAGE_X2-1, 1005)) draw_village_sprite(VILLAGE_X2-1, 1005, 7);
    // 마을 NPC 4명 + 머리 위 라벨
    int npc_count = (int)(sizeof(g_village_npcs) / sizeof(g_village_npcs[0]));
    for (int ni = 0; ni < npc_count; ++ni) {
        const auto& npc = g_village_npcs[ni];
        if (!in_view_tile(npc.x, npc.y)) continue;
        draw_village_sprite(npc.x, npc.y, npc.sprite_col);
        // Stage 9: 장로(0)는 보고 가능 퀘스트가 있으면 "?"(금색)로 강조
        const char* label = npc.label;
        sf::Color label_color = sf::Color::Yellow;
        if (ni == 0 && quest_has_turnin_ready()) { label = "?"; label_color = sf::Color(255, 215, 90); }
        sf::Text t;
        t.setFont(*g_font);
        t.setString(label);
        t.setCharacterSize(20);
        t.setFillColor(label_color);
        t.setOutlineColor(sf::Color::Black);
        t.setOutlineThickness(2.0f);
        t.setStyle(sf::Text::Bold);
        sf::FloatRect b = t.getLocalBounds();
        t.setOrigin(b.left + b.width / 2.0f, b.top + b.height / 2.0f);
        float px = (npc.x - g_left_x + 0.5f) * (float)TILE_WIDTH;
        float py = (npc.y - g_top_y) * (float)TILE_WIDTH - 6.0f;
        t.setPosition(px, py);
        g_window->draw(t);
    }

    // === Stage 6.4: 4지역 장식 + 랜드마크/보스 sprite (캐릭터 아래에 배치) ===
    auto draw_landmark_sprite = [&](const WorldDeco& d) {
        if (!in_view_tile(d.x, d.y)) return;
        landmark_sprite.setTextureRect(sf::IntRect(d.sprite_col * 32, 0, 32, 32));
        float px = (d.x - g_left_x) * (float)TILE_WIDTH;
        float py = (d.y - g_top_y)  * (float)TILE_WIDTH;
        landmark_sprite.setPosition(px, py);
        g_window->draw(landmark_sprite);
    };
    for (const auto& d : g_decorations) draw_landmark_sprite(d);
    for (const auto& d : g_landmarks)   draw_landmark_sprite(d);

    // === Stage 8: 바닥 아이템 (캐릭터 아래) — DCSS 아이템 스프라이트 ===
    for (const auto& kv : g_ground_items) {
        const GroundItemView& gi = kv.second;
        if (!in_view_tile(gi.x, gi.y)) continue;
        float cx = (gi.x - g_left_x + 0.5f) * (float)TILE_WIDTH;
        float cy = (gi.y - g_top_y  + 0.5f) * (float)TILE_WIDTH;
        // 바닥 그림자(타원)로 드롭이 떠 있는 느낌 강조
        sf::CircleShape shadow(TILE_WIDTH * 0.26f);
        shadow.setScale(1.0f, 0.45f);
        shadow.setOrigin(shadow.getRadius(), shadow.getRadius());
        shadow.setPosition(cx, cy + TILE_WIDTH * 0.28f);
        shadow.setFillColor(sf::Color(0, 0, 0, 90));
        g_window->draw(shadow);
        draw_item_sprite(gi.item_id, cx, cy, (float)TILE_WIDTH * 0.9f);
    }

    avatar.draw();
    for (auto& pl : players) pl.second.draw();

    // 활성 이펙트 렌더링 (캐릭터 위에 표시)
    for (auto& eff : g_effects) eff.draw();
    // 만료된 이펙트 제거
    g_effects.erase(
        std::remove_if(g_effects.begin(), g_effects.end(),
                       [](const Effect& e) { return e.is_done(); }),
        g_effects.end());

    // === 마을 라벨 (Aetheria Village + 입구 4방향) — 캐릭터/이펙트 위에 ===
    {
        auto draw_world_text = [&](int wx, int wy, const std::string& s,
                                    unsigned int size, sf::Color color) {
            // 시야 밖이면 스킵
            if (wx < g_left_x - 2 || wx >= g_left_x + SCREEN_WIDTH + 2 ||
                wy < g_top_y  - 2 || wy >= g_top_y  + SCREEN_HEIGHT + 2) return;
            sf::Text t;
            t.setFont(*g_font);
            t.setString(s);
            t.setCharacterSize(size);
            t.setFillColor(color);
            t.setOutlineColor(sf::Color::Black);
            t.setOutlineThickness(2.0f);
            sf::FloatRect b = t.getLocalBounds();
            t.setOrigin(b.left + b.width / 2.0f, b.top + b.height / 2.0f);
            float cx = (wx - g_left_x + 0.5f) * TILE_WIDTH;
            float cy = (wy - g_top_y  + 0.5f) * TILE_WIDTH;
            t.setPosition(cx, cy);
            g_window->draw(t);
        };
        // 마을 이름 (상단 안쪽)
        draw_world_text(FOUNTAIN_X, VILLAGE_Y1 + 5, "Aetheria Village", 22, sf::Color::White);
        // 입구 라벨 4방향 — RPG 진행 흐름 (Lv 순)
        draw_world_text(FOUNTAIN_X,     VILLAGE_Y1 - 2, "^ Desert/Snow (Lv 20+)", 14, sf::Color(255, 220, 170));
        draw_world_text(FOUNTAIN_X,     VILLAGE_Y2 + 1, "v Plain/Grove (Lv 1+)",  14, sf::Color(170, 255, 170));
        draw_world_text(VILLAGE_X1 - 4, FOUNTAIN_Y,     "< Snow/Plain",           14, sf::Color(190, 230, 255));
        draw_world_text(VILLAGE_X2 + 3, FOUNTAIN_Y,     "Desert/Grove >",         14, sf::Color(255, 200, 140));

        // 4지역 랜드마크/보스 라벨 (sprite 위쪽 1타일에 표시)
        for (const auto& d : g_landmarks) {
            if (d.label[0] == '\0') continue;
            sf::Color lc = d.is_boss ? sf::Color(255, 100, 100) : sf::Color(255, 230, 130);
            draw_world_text(d.x, d.y - 1, d.label, 16, lc);
        }
    }

    // 데미지 숫자 popup 그리기 (흔들리는 view 상태에서 — 라벨과 함께 흔들림)
    for (auto& d : g_dmg_popups) d.draw();
    g_dmg_popups.erase(
        std::remove_if(g_dmg_popups.begin(), g_dmg_popups.end(),
                       [](const FloatingDamage& d) { return d.is_done(); }),
        g_dmg_popups.end());

    // HUD는 흔들리지 않게 기본 view로 복원
    g_window->setView(default_view);

    // 좌표 텍스트는 미니맵 하단 헤더로 이동 — 별도 그리기 제거
    draw_minimap();
    draw_hud();
}

int main()
{
    wcout.imbue(locale("korean"));
    std::cout << "Enter User Name : ";
    std::cin >> avatar_name;
    std::string server_ip;
    std::cout << "Enter Server IP (default: 127.0.0.1) : ";
    std::cin.ignore();
    std::getline(std::cin, server_ip);
    if (server_ip.empty()) server_ip = "127.0.0.1";
    sf::Socket::Status status = socket.connect(server_ip, PORT);
    socket.setBlocking(false);

    if (status != sf::Socket::Done) while (true);

    client_initialize();

    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT - 16x16 View");
    g_window = &window;

    while (window.isOpen())
    {
        sf::Event event;
        while (window.pollEvent(event))
        {
            if (event.type == sf::Event::Closed) window.close();

            // 채팅 입력 모드: 타이핑 문자 처리 (TextEntered 이벤트)
            if (event.type == sf::Event::TextEntered && g_chat_input_mode) {
                sf::Uint32 ch = event.text.unicode;
                if (ch == 8) {  // Backspace
                    if (!g_chat_buffer.empty()) g_chat_buffer.pop_back();
                }
                else if (ch >= 32 && ch < 127) {  // 인쇄 가능 ASCII
                    if ((int)g_chat_buffer.size() < CHAT_BUFFER_MAX) {
                        g_chat_buffer += static_cast<char>(ch);
                    }
                }
                // 13(CR), 10(LF) 등은 KeyPressed에서 Enter로 처리
                continue;
            }

            if (event.type == sf::Event::KeyPressed) {
                // 채팅 모드: Escape=취소. Enter는 이벤트 루프 밖에서 폴링으로 처리 (IME/키반복 이슈 회피).
                if (g_chat_input_mode) {
                    if (event.key.code == sf::Keyboard::Escape) {
                        g_chat_buffer.clear();
                        g_chat_input_mode = false;
                    }
                    continue;
                }

                // Stage 9: 퀘스트 대화창이 열린 동안엔 Y=확정 / Escape=닫기만 처리 (모달)
                if (g_dialogue_open) {
                    if (event.key.code == sf::Keyboard::Y) {
                        if (g_dialogue_kind == 0 || g_dialogue_kind == 2) {  // Offer 또는 ReadyTurnIn
                            C2S_QuestAction qa;
                            qa.size = sizeof(qa);
                            qa.type = C2S_QUEST_ACTION;
                            qa.quest_id = g_dialogue_quest;
                            qa.action = (g_dialogue_kind == 0) ? 0 : 1;  // 0=accept,1=turnin
                            send_packet(&qa);
                        }
                        g_dialogue_open = false;
                    }
                    else if (event.key.code == sf::Keyboard::Escape ||
                             event.key.code == sf::Keyboard::T) {
                        g_dialogue_open = false;
                    }
                    continue;  // 대화창 동안 다른 입력 차단
                }

                // Stage 8: 인벤토리 패널이 열린 동안엔 방향키/장착 키를 인벤 조작으로 가로챔 (모달)
                if (g_inv_open) {
                    constexpr int INV_COLS = 5;  // 인벤 그리드 열 수 (draw의 COLS와 동일)
                    // 슬롯 사용(소모품)/장착(무기·방어구) 공통 처리
                    auto use_or_equip = [&](int slot) {
                        if (slot < 0 || slot >= (int)g_inventory.size()) return;
                        int item_id = g_inventory[slot].first;
                        const ClientItemMeta* m = item_meta(item_id);
                        if (m && m->cat == CIC_CONSUMABLE) {
                            C2S_UseItem u; u.size = sizeof(u); u.type = C2S_USE_ITEM; u.slot = (unsigned char)slot; send_packet(&u);
                        } else if (m) {
                            C2S_EquipItem e; e.size = sizeof(e); e.type = C2S_EQUIP_ITEM; e.slot = (unsigned char)slot; send_packet(&e);
                        }
                    };
                    int slot = -1;
                    switch (event.key.code) {
                    // 방향키 = 커서 이동 (20칸 5열 그리드)
                    case sf::Keyboard::Left:  if (g_inv_cursor % INV_COLS > 0) g_inv_cursor -= 1; break;
                    case sf::Keyboard::Right: if (g_inv_cursor % INV_COLS < INV_COLS - 1 &&
                                                  g_inv_cursor + 1 < MAX_INVENTORY_SLOTS) g_inv_cursor += 1; break;
                    case sf::Keyboard::Up:     if (g_inv_cursor - INV_COLS >= 0) g_inv_cursor -= INV_COLS; break;
                    case sf::Keyboard::Down:   if (g_inv_cursor + INV_COLS < MAX_INVENTORY_SLOTS) g_inv_cursor += INV_COLS; break;
                    // F / Enter = 커서가 가리키는 슬롯 사용·장착
                    case sf::Keyboard::F:
                    case sf::Keyboard::Enter:  use_or_equip(g_inv_cursor); break;
                    // 1~0 = 앞 10칸 빠른 사용·장착 (기존 단축키 유지)
                    case sf::Keyboard::Num1: slot = 0; break;
                    case sf::Keyboard::Num2: slot = 1; break;
                    case sf::Keyboard::Num3: slot = 2; break;
                    case sf::Keyboard::Num4: slot = 3; break;
                    case sf::Keyboard::Num5: slot = 4; break;
                    case sf::Keyboard::Num6: slot = 5; break;
                    case sf::Keyboard::Num7: slot = 6; break;
                    case sf::Keyboard::Num8: slot = 7; break;
                    case sf::Keyboard::Num9: slot = 8; break;
                    case sf::Keyboard::Num0: slot = 9; break;
                    case sf::Keyboard::I:
                    case sf::Keyboard::Escape:
                        g_inv_open = false; break;
                    case sf::Keyboard::G: {  // 패널 열린 채로도 줍기 가능
                        C2S_PickUp pk; pk.size = sizeof(pk); pk.type = C2S_PICKUP; send_packet(&pk);
                        break;
                    }
                    case sf::Keyboard::R: {  // 무기 해제
                        C2S_UnequipItem u; u.size = sizeof(u); u.type = C2S_UNEQUIP_ITEM; u.which = 0; send_packet(&u);
                        break;
                    }
                    case sf::Keyboard::A: {  // 방어구 해제 (F가 사용·장착으로 바뀌어 A로 이동)
                        C2S_UnequipItem u; u.size = sizeof(u); u.type = C2S_UNEQUIP_ITEM; u.which = 1; send_packet(&u);
                        break;
                    }
                    case sf::Keyboard::D: {  // 커서 슬롯 아이템 버리기 (발밑 바닥에 떨어뜨림)
                        if (g_inv_cursor >= 0 && g_inv_cursor < (int)g_inventory.size()) {
                            C2S_DropItem d; d.size = sizeof(d); d.type = C2S_DROP_ITEM; d.slot = (unsigned char)g_inv_cursor; send_packet(&d);
                        }
                        break;
                    }
                    default: break;
                    }
                    if (g_inv_cursor < 0) g_inv_cursor = 0;
                    if (g_inv_cursor >= MAX_INVENTORY_SLOTS) g_inv_cursor = MAX_INVENTORY_SLOTS - 1;
                    if (slot >= 0) use_or_equip(slot);
                    continue;  // 인벤 모드에선 이동/공격/스킬 입력 무시
                }

                short target_x = avatar.m_x;
                short target_y = avatar.m_y;
                bool moved = false;
                bool teleported = false;

                switch (event.key.code) {
                case sf::Keyboard::Left:  target_x -= 1; moved = true; break;
                case sf::Keyboard::Right: target_x += 1; moved = true; break;
                case sf::Keyboard::Up:    target_y -= 1; moved = true; break;
                case sf::Keyboard::Down:  target_y += 1; moved = true; break;
                // 테스트용 텔레포트: 1=상 / 2=하 / 3=좌 / 4=우, 10칸씩 이동
                case sf::Keyboard::Num1:  target_y -= 10; teleported = true; break;
                case sf::Keyboard::Num2:  target_y += 10; teleported = true; break;
                case sf::Keyboard::Num3:  target_x -= 10; teleported = true; break;
                case sf::Keyboard::Num4:  target_x += 10; teleported = true; break;
                // 보스 앞으로 텔레포트: 5=SW초원 GrassBoss / 6=SE숲 ForestBoss / 7=NE사막 DesertBoss / 8=NW설원 IceBoss
                // (500,1402)는 절차적 장식(Map::IsProceduralDeco)으로 막혀 서버가 텔레포트를 거부하므로
                // 보스(500,1400) 바로 앞 walkable 타일 (500,1401)로 지정.
                case sf::Keyboard::Num5:  target_x = 500;  target_y = 1401; teleported = true; break;
                case sf::Keyboard::Num6:  target_x = 1500; target_y = 1402; teleported = true; break;
                case sf::Keyboard::Num7:  target_x = 1500; target_y = 402;  teleported = true; break;
                case sf::Keyboard::Num8:  target_x = 500;  target_y = 402;  teleported = true; break;
                // 9=마을(스폰지점)으로 복귀
                case sf::Keyboard::Num9:  target_x = 1000; target_y = 1002; teleported = true; break;
                // A키 = 공격 (인접 4타일 NPC 동시 데미지). 서버가 쿨타임 검증.
                case sf::Keyboard::A: {
                    C2S_Attack ap;
                    ap.size = sizeof(ap);
                    ap.type = C2S_ATTACK;
                    send_packet(&ap);
                    break;
                }
                // Q = 스킬 1 (AoE 3칸 반경), W = 스킬 2 (방향 직선 5칸), E = 스킬 3 (Heal 30%)
                // 쿨타임 중이면 완전히 무시 (키 반복으로 KeyPressed가 계속 발생해도 1회만 처리)
                case sf::Keyboard::Q: {
                    bool on_cd = g_skill1_used &&
                        g_skill1_clock.getElapsedTime().asMilliseconds() < SKILL1_CD_MS;
                    if (!on_cd) {
                        C2S_UseSkill sk; sk.size = sizeof(sk); sk.type = C2S_USE_SKILL; sk.skill_id = 1;
                        send_packet(&sk);
                        g_skill1_clock.restart(); g_skill1_used = true;
                        if (g_sfx_q.getBuffer()) g_sfx_q.play();
                    }
                    break;
                }
                case sf::Keyboard::W: {
                    bool on_cd = g_skill2_used &&
                        g_skill2_clock.getElapsedTime().asMilliseconds() < SKILL2_CD_MS;
                    if (!on_cd) {
                        C2S_UseSkill sk; sk.size = sizeof(sk); sk.type = C2S_USE_SKILL; sk.skill_id = 2;
                        send_packet(&sk);
                        g_skill2_clock.restart(); g_skill2_used = true;
                        if (g_sfx_w.getBuffer()) g_sfx_w.play();
                    }
                    break;
                }
                case sf::Keyboard::E: {
                    bool on_cd = g_skill3_used &&
                        g_skill3_clock.getElapsedTime().asMilliseconds() < SKILL3_CD_MS;
                    if (!on_cd) {
                        C2S_UseSkill sk; sk.size = sizeof(sk); sk.type = C2S_USE_SKILL; sk.skill_id = 3;
                        send_packet(&sk);
                        g_skill3_clock.restart(); g_skill3_used = true;
                        if (g_sfx_e.getBuffer()) g_sfx_e.play();
                    }
                    break;
                }
                // Stage 8: I=인벤토리 열기, G=근처 바닥 아이템 줍기
                case sf::Keyboard::I: g_inv_open = true; break;
                case sf::Keyboard::G: {
                    C2S_PickUp pk; pk.size = sizeof(pk); pk.type = C2S_PICKUP; send_packet(&pk);
                    break;
                }
                // Stage 9: T=장로와 대화(근처에서만), J=퀘스트 로그 토글
                case sf::Keyboard::T: {
                    int ndx = abs((int)avatar.m_x - g_village_npcs[0].x);
                    int ndy = abs((int)avatar.m_y - g_village_npcs[0].y);
                    if (std::max(ndx, ndy) <= 3) {
                        C2S_QuestInteract qi;
                        qi.size = sizeof(qi);
                        qi.type = C2S_QUEST_INTERACT;
                        qi.npc_index = 0;
                        send_packet(&qi);
                    } else {
                        add_chat_line("[Quest] 장로 근처(마을 북서쪽)에서 T로 대화하세요.");
                    }
                    break;
                }
                case sf::Keyboard::J: g_quest_log_open = !g_quest_log_open; break;
                case sf::Keyboard::Escape: window.close(); break;
                }

                if (teleported) {
                    // 서버가 월드 경계로 clamp하므로 클라이언트에서 보정 불필요
                    C2S_Teleport tp;
                    tp.size = sizeof(tp);
                    tp.type = C2S_TELEPORT;
                    tp.x = target_x;
                    tp.y = target_y;
                    send_packet(&tp);
                    continue;
                }

                if (moved) {
                    // OS 키 반복으로 KeyPressed가 초당 ~30회 발생해 서버 쿨타임(500ms)을
                    // 초과하는 패킷을 쏟아내면 desync가 생기므로 클라이언트에서 동일 쿨타임 적용.
                    static auto last_move_send = std::chrono::steady_clock::time_point{};
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_move_send).count();
                    if (elapsed < CLIENT_MOVE_COOLDOWN_MS) {
                        continue; // 쿨타임 미충족: 이벤트 무시
                    }
                    last_move_send = now;

                    C2S_Move p;
                    p.size = sizeof(p);
                    p.type = C2S_MOVE;
                    p.x = target_x;
                    p.y = target_y;
                    p.move_time = 0; // 아직 서버에 타이머가 없으므로 0
                    send_packet(&p);
                }
            }
        }

        // 이벤트 루프 밖에서 Enter 키 상태를 폴링 — 한국어 IME나 키 자동반복 같이
        // 이벤트 발화가 일관되지 않는 환경에서도 안정적으로 토글을 검출
        {
            static bool enter_was_down = false;
            bool enter_now = sf::Keyboard::isKeyPressed(sf::Keyboard::Enter);
            if (enter_now && !enter_was_down) {
                // 키 down 엣지 — 한 번만 발화
                if (g_chat_input_mode) {
                    if (!g_chat_buffer.empty()) {
                        const std::string& msg = g_chat_buffer;

                        // 파티 명령어 처리: /invite <name>, /accept, /reject, /leave
                        if (msg.rfind("/invite ", 0) == 0 && msg.size() > 8) {
                            std::string target = msg.substr(8);
                            C2S_PartyInvite pk;
                            pk.size = sizeof(pk);
                            pk.type = C2S_PARTY_INVITE;
                            strncpy_s(pk.target_name, sizeof(pk.target_name), target.c_str(), _TRUNCATE);
                            send_packet(&pk);
                        } else if (msg == "/accept") {
                            C2S_PartyAccept pk; pk.size = sizeof(pk); pk.type = C2S_PARTY_ACCEPT;
                            send_packet(&pk);
                        } else if (msg == "/reject") {
                            C2S_PartyReject pk; pk.size = sizeof(pk); pk.type = C2S_PARTY_REJECT;
                            send_packet(&pk);
                        } else if (msg == "/leave") {
                            C2S_PartyLeave pk; pk.size = sizeof(pk); pk.type = C2S_PARTY_LEAVE;
                            send_packet(&pk);
                        } else {
                            // 일반 채팅
                            C2S_Chat cp;
                            cp.size = sizeof(cp);
                            cp.type = C2S_CHAT;
                            strncpy_s(cp.message, sizeof(cp.message), msg.c_str(), _TRUNCATE);
                            cp.message[MAX_CHAT_MSG_LEN - 1] = '\0';
                            send_packet(&cp);
                        }
                    }
                    g_chat_buffer.clear();
                    g_chat_input_mode = false;
                }
                else {
                    g_chat_buffer.clear();
                    g_chat_input_mode = true;
                }
            }
            enter_was_down = enter_now;
        }

        window.clear();
        client_main();
        window.display();
    }
    client_finish();

    return 0;
}