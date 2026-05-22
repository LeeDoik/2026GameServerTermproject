#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <iostream>
#include <unordered_map>
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
constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = SCREEN_WIDTH * TILE_WIDTH;
constexpr auto WINDOW_HEIGHT = SCREEN_HEIGHT * TILE_WIDTH;
constexpr int BUF_SIZE = 1024;

int g_left_x;
int g_top_y;
int g_myid;

// 플레이어 자신의 스탯 (HUD 표시용). S2C_AVATAR_INFO / S2C_StatusChange로 갱신.
int g_my_hp = 100;
int g_my_max_hp = 100;
unsigned long long g_my_exp = 0;
unsigned char g_my_level = 1;

// 채팅
bool g_chat_input_mode = false;
std::string g_chat_buffer;
constexpr int CHAT_BUFFER_MAX = MAX_CHAT_MSG_LEN - 1;
constexpr int CHAT_LOG_MAX = 6;  // 화면에 표시할 최근 메시지 수
std::vector<std::string> g_chat_log;

static void add_chat_line(const std::string& s) {
    g_chat_log.push_back(s);
    if ((int)g_chat_log.size() > CHAT_LOG_MAX) {
        g_chat_log.erase(g_chat_log.begin(),
                         g_chat_log.begin() + ((int)g_chat_log.size() - CHAT_LOG_MAX));
    }
}

sf::RenderWindow* g_window;
sf::Font* g_font;

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
    int m_direction = HERO_DIR_DOWN;
    int m_frame_idx = 0;       // 0~3, walk cycle
    sf::Clock m_idle_timer;    // 마지막 이동 이후 경과 시간

    // 공격 모션 (워리어 전용, attack_tex가 있을 때만)
    sf::Texture* m_walk_tex = nullptr;
    sf::Texture* m_attack_tex = nullptr;
    bool m_attacking = false;
    int m_attack_dir = HERO_DIR_DOWN;
    sf::Clock m_attack_clock;

public:
    int m_x, m_y;
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

        float rx = (m_x - g_left_x) * 65.0f + 1;
        float ry = (m_y - g_top_y) * 65.0f + 1;
        m_sprite.setPosition(rx, ry);
        g_window->draw(m_sprite);
        auto size = m_name.getGlobalBounds();
        m_name.setPosition(rx + 32 - size.width / 2, ry - 10);
        g_window->draw(m_name);
    }

    void set_name(const char str[]) {
        strncpy_s(name, sizeof(name), str, _TRUNCATE);
        m_name.setFont(*g_font);
        m_name.setString(str);
        m_name.setCharacterSize(20);
        m_name.setFillColor(sf::Color(255, 255, 0));
        m_name.setStyle(sf::Text::Bold);
    }
};

OBJECT avatar;
std::unordered_map <int, OBJECT> players;
std::string avatar_name;

// 던전 타일맵: 256x256 PNG, 4행x4열, 각 64x64.
// 행 = 지역 종류 (0:NW균열석 / 1:SW혈흔 / 2:NE벽돌 / 3:SE룬), 열 = 같은 지역의 4가지 variant.
sf::Texture* dungeon_tiles;
sf::Sprite tile_sprite;
constexpr int TILE_SRC_SIZE = 64;          // 원본 픽셀
constexpr int REGION_HALF   = WORLD_WIDTH / 2;  // 2000/2 = 1000 (지역 경계)

// 4방향 walk 스프라이트 시트 (모두 동일 레이아웃: 256x256, Row=방향, Col=walk프레임)
sf::Texture* hero_tex;        // 플레이어 워리어 walk
sf::Texture* hero_attack_tex; // 플레이어 워리어 attack (192x256, 4행x3열, 각 64x64)
sf::Texture* orc_tex;         // NPC: Agro Orc

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

sf::Sprite hud_sprite;       // HUD 그리기용 재사용 sprite

// 화면에 떠 있는 이펙트 1개. 시트 한 행을 프레임 순서대로 재생 후 자동 소멸.
struct Effect {
    sf::Sprite sprite;
    int frame_count;
    int frame_w, frame_h;
    int duration_ms;
    int world_x, world_y;     // 월드 타일 좌표
    int offset_x, offset_y;    // 그릴 때 타일 좌상단 기준 픽셀 보정
    sf::Clock clock;

    bool is_done() const {
        return clock.getElapsedTime().asMilliseconds() >= duration_ms;
    }
    int current_frame() {
        long long ms = clock.getElapsedTime().asMilliseconds();
        int f = (int)(ms * frame_count / duration_ms);
        if (f < 0) f = 0;
        if (f >= frame_count) f = frame_count - 1;
        return f;
    }
    void draw() {
        // 시야 밖이면 스킵 (이펙트는 진행하되 화면에는 안 그림)
        if (world_x < g_left_x || world_x >= g_left_x + SCREEN_WIDTH ||
            world_y < g_top_y || world_y >= g_top_y + SCREEN_HEIGHT) return;
        int f = current_frame();
        sprite.setTextureRect(sf::IntRect(f * frame_w, 0, frame_w, frame_h));
        float rx = (world_x - g_left_x) * 65.0f + 1.0f + offset_x;
        float ry = (world_y - g_top_y) * 65.0f + 1.0f + offset_y;
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
    dungeon_tiles = new sf::Texture;
    hero_tex      = new sf::Texture;
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
    slash_tex     = new sf::Texture;

    if (!LoadTextureWithFallback(dungeon_tiles,  "dungeon-tiles-256x256.png",        "tiles"))     exit(-1);
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
    if (!LoadTextureWithFallback(slash_tex,      "slash-effect-576x96.png",          "effects"))   exit(-1);

    tile_sprite.setTexture(*dungeon_tiles);
    // 원본 64px 타일을 화면 65px 칸에 맞춰 스케일 (1px 갭/겹침 방지)
    tile_sprite.setScale((float)TILE_WIDTH / TILE_SRC_SIZE, (float)TILE_WIDTH / TILE_SRC_SIZE);

    // exp fill은 가로로 반복해서 채워야 하므로 wrap repeat 활성화
    exp_fill_tex->setRepeated(true);

    g_font = new sf::Font;
    if (false == g_font->loadFromFile("cour.ttf")) {
        cout << "Font Loading Error!\n";
        exit(-1);
    }
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
            players[id].set_hero(*orc_tex);   // NPC = 빨간 오크 (공격 모션 없음)
        }
        else {
            players[id].set_hero(*hero_tex, hero_attack_tex);  // 다른 플레이어 = 워리어
        }
        players[id].move(my_packet->x, my_packet->y);
        players[id].set_name(my_packet->obj_name);
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
        // 공격 위치(자기 자리)에 슬래시 이펙트도 같이 그림
        if (aid == g_myid) {
            avatar.on_attack(dir);
            spawn_effect_slash(avatar.m_x, avatar.m_y);
        }
        else {
            auto it = players.find(aid);
            if (it != players.end()) {
                it->second.on_attack(dir);
                spawn_effect_slash(it->second.m_x, it->second.m_y);
            }
        }
        break;
    }
    case S2C_DAMAGE:
    {
        S2C_Damage* p = reinterpret_cast<S2C_Damage*>(ptr);
        // 혈흔 이펙트는 target 위치에 (공격자가 아닌 맞은 쪽)
        spawn_effect_blood(p->target_x, p->target_y);
        // HP는 자기가 맞은 경우만 갱신 (NPC HP는 클라가 직접 추적 안 함)
        if (p->target_id == g_myid) {
            g_my_hp = p->new_hp;
        }
        break;
    }
    case S2C_STATUS_CHANGE:
    {
        S2C_StatusChange* p = reinterpret_cast<S2C_StatusChange*>(ptr);
        if (p->object_id == g_myid) {
            g_my_hp = p->hp;
            g_my_max_hp = p->max_hp;
            g_my_exp = p->exp;
            g_my_level = p->level;
        }
        break;
    }
    case S2C_DEATH:
    {
        S2C_Death* p = reinterpret_cast<S2C_Death*>(ptr);
        spawn_effect_death(p->death_x, p->death_y);
        if (p->object_id == g_myid) {
            avatar.hide();  // Phase 3b에서 플레이어 사망/리스폰 처리 추가 예정
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
        if (p->object_id == g_myid) {
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

// HUD 그리기 (월드 렌더 위에 오버레이). 모든 값은 g_my_* 전역에서 읽음.
// Stage 5에서 S2C_StatusChange로 hp/exp가 업데이트되면 자동으로 HUD에도 반영됨.
static void draw_hud()
{
    // 비율 계산 (max 0 가드)
    float hp_ratio  = (g_my_max_hp > 0) ? std::min(1.0f, (float)g_my_hp / (float)g_my_max_hp) : 0.0f;
    float mp_ratio  = 1.0f;  // MP 시스템은 아직 없음 — 일단 가득
    unsigned long long max_exp = (unsigned long long)g_my_level * g_my_level * 2ull;
    if (max_exp == 0) max_exp = 1;
    float exp_ratio = std::min(1.0f, (float)g_my_exp / (float)max_exp);

    // ---- 채팅 패널 (우상단, 512x192 → 320x120) ----
    constexpr float CHAT_W = 320.0f, CHAT_H = 120.0f;
    const float CHAT_X = WINDOW_WIDTH - CHAT_W - 10.0f;
    const float CHAT_Y = 10.0f;
    {
        hud_sprite.setTexture(*chat_panel_tex, true);
        hud_sprite.setTextureRect(sf::IntRect(0, 0, 512, 192));
        hud_sprite.setScale(CHAT_W / 512.0f, CHAT_H / 192.0f);
        hud_sprite.setPosition(CHAT_X, CHAT_Y);
        g_window->draw(hud_sprite);
    }

    // 채팅 로그 (최근 메시지를 위에서부터 표시)
    {
        sf::Text txt;
        txt.setFont(*g_font);
        txt.setCharacterSize(13);
        txt.setFillColor(sf::Color(245, 235, 200));
        txt.setOutlineColor(sf::Color::Black);
        txt.setOutlineThickness(1.0f);
        const float line_h = 15.0f;
        const float text_pad_x = 14.0f;
        const float text_pad_y = 8.0f;
        for (size_t i = 0; i < g_chat_log.size(); ++i) {
            txt.setString(g_chat_log[i]);
            txt.setPosition(CHAT_X + text_pad_x, CHAT_Y + text_pad_y + i * line_h);
            g_window->draw(txt);
        }
    }

    // 입력 박스 (채팅 모드일 때만, 패널 바로 아래)
    if (g_chat_input_mode) {
        const float input_h = 22.0f;
        const float input_y = CHAT_Y + CHAT_H + 4.0f;
        sf::RectangleShape box(sf::Vector2f(CHAT_W, input_h));
        box.setFillColor(sf::Color(10, 8, 20, 230));
        box.setOutlineColor(sf::Color(230, 190, 90));
        box.setOutlineThickness(2.0f);
        box.setPosition(CHAT_X, input_y);
        g_window->draw(box);

        sf::Text inp;
        inp.setFont(*g_font);
        inp.setCharacterSize(14);
        inp.setFillColor(sf::Color::White);
        // 깜빡이는 커서 (~0.5초 주기)
        static sf::Clock cursor_clock;
        bool show_cursor = (cursor_clock.getElapsedTime().asMilliseconds() / 500) % 2 == 0;
        inp.setString("> " + g_chat_buffer + (show_cursor ? "_" : " "));
        inp.setPosition(CHAT_X + 6.0f, input_y + 2.0f);
        g_window->draw(inp);
    }

    // ---- HP/MP 구슬 (좌하단/우하단, 각 256x256 → 128x128) ----
    // 게이지를 원형(ConvexShape)으로 그려서 프레임의 원형 inner area에 정확히 맞춤
    constexpr float ORB_SIZE     = 128.0f;
    constexpr float ORB_INNER_R  = 44.0f;  // 게이지의 반지름 — 프레임 안쪽 원과 일치
    float hp_x = 10.0f;
    float mp_x = WINDOW_WIDTH - ORB_SIZE - 10.0f;
    float orb_y = WINDOW_HEIGHT - ORB_SIZE - 50.0f;  // EXP 바 위에 50px 여유

    sf::Color empty_bg(20, 10, 10, 240);  // 매우 어두운 적자색 (액체 비어있을 때 보임)

    // HP 게이지 (빨강) — 좌측 구슬
    draw_circular_gauge(hp_x + ORB_SIZE * 0.5f, orb_y + ORB_SIZE * 0.5f, ORB_INNER_R,
                        hp_ratio, sf::Color(180, 30, 30), empty_bg);
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

        // 채움 (안쪽 영역만, 비율만큼 — fill tile을 가로 반복하며 늘림)
        if (exp_ratio > 0.0f) {
            float fill_pixel_w = (BAR_W - INNER_PAD_X * 2.0f) * exp_ratio;
            float fill_pixel_h = BAR_H - INNER_PAD_Y * 2.0f;
            sf::Sprite fill;
            fill.setTexture(*exp_fill_tex);
            // setRepeated(true)와 함께 IntRect 너비를 늘리면 가로로 타일링
            fill.setTextureRect(sf::IntRect(0, 0, (int)(fill_pixel_w / (fill_pixel_h / 32.0f)), 32));
            fill.setScale(fill_pixel_h / 32.0f, fill_pixel_h / 32.0f);
            fill.setPosition(bar_x + INNER_PAD_X, bar_y + INNER_PAD_Y);
            g_window->draw(fill);
        }

        // 프레임
        hud_sprite.setTexture(*exp_frame_tex, true);
        hud_sprite.setTextureRect(sf::IntRect(0, 0, 1024, 48));
        hud_sprite.setScale(BAR_W / 1024.0f, BAR_H / 48.0f);
        hud_sprite.setPosition(bar_x, bar_y);
        g_window->draw(hud_sprite);
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

    for (int i = 0; i < SCREEN_WIDTH; ++i) {
        for (int j = 0; j < SCREEN_HEIGHT; ++j)
        {
            int tile_x = i + g_left_x;
            int tile_y = j + g_top_y;

            if ((tile_x < 0) || (tile_y < 0) || (tile_x >= WORLD_WIDTH) || (tile_y >= WORLD_HEIGHT)) continue;

            // 지역 결정: 2000x2000을 4분할 → 행 인덱스(0~3) = 타일시트의 row
            //   NW(x<1000,y<1000)=0 균열석 / SW(x<1000,y>=1000)=1 혈흔
            //   NE(x>=1000,y<1000)=2 벽돌  / SE(x>=1000,y>=1000)=3 룬
            int region_row = (tile_x >= REGION_HALF ? 2 : 0) + (tile_y >= REGION_HALF ? 1 : 0);

            // variant: 같은 (wx,wy)는 항상 같은 variant (좌표 해시) → 패턴이 자연스럽게 섞임
            unsigned int h = (unsigned int)(tile_x * 73856093) ^ (unsigned int)(tile_y * 19349663);
            int variant_col = h & 0x3;

            tile_sprite.setTextureRect(sf::IntRect(
                variant_col * TILE_SRC_SIZE, region_row * TILE_SRC_SIZE,
                TILE_SRC_SIZE, TILE_SRC_SIZE));
            tile_sprite.setPosition((float)(TILE_WIDTH * i), (float)(TILE_WIDTH * j));
            g_window->draw(tile_sprite);
        }
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

    sf::Text text;
    text.setFont(*g_font);
    char buf[100];
    sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
    text.setString(buf);
    text.setFillColor(sf::Color::White);
    text.setOutlineColor(sf::Color::Black);
    text.setOutlineThickness(2.0f);
    g_window->draw(text);

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

                short target_x = avatar.m_x;
                short target_y = avatar.m_y;
                bool moved = false;
                bool teleported = false;

                switch (event.key.code) {
                case sf::Keyboard::Left:  target_x -= 1; moved = true; break;
                case sf::Keyboard::Right: target_x += 1; moved = true; break;
                case sf::Keyboard::Up:    target_y -= 1; moved = true; break;
                case sf::Keyboard::Down:  target_y += 1; moved = true; break;
                // 테스트용 텔레포트: 1=상 / 2=하 / 3=좌 / 4=우, 100칸씩 이동
                case sf::Keyboard::Num1:  target_y -= 100; teleported = true; break;
                case sf::Keyboard::Num2:  target_y += 100; teleported = true; break;
                case sf::Keyboard::Num3:  target_x -= 100; teleported = true; break;
                case sf::Keyboard::Num4:  target_x += 100; teleported = true; break;
                // 디버그용 이펙트 트리거: 5=혈흔 / 6=사망 / 7=리스폰 / 8=레벨업 / 9=슬래시
                case sf::Keyboard::Num5:  spawn_effect_blood(avatar.m_x, avatar.m_y);   break;
                case sf::Keyboard::Num6:  spawn_effect_death(avatar.m_x, avatar.m_y);   break;
                case sf::Keyboard::Num7:  spawn_effect_respawn(avatar.m_x, avatar.m_y); break;
                case sf::Keyboard::Num8:  spawn_effect_levelup(avatar.m_x, avatar.m_y); break;
                case sf::Keyboard::Num9:  spawn_effect_slash(avatar.m_x, avatar.m_y);   break;
                // A키 = 공격 (인접 4타일 NPC 동시 데미지). 서버가 쿨타임 검증.
                case sf::Keyboard::A: {
                    C2S_Attack ap;
                    ap.size = sizeof(ap);
                    ap.type = C2S_ATTACK;
                    send_packet(&ap);
                    break;
                }
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
                        C2S_Chat cp;
                        cp.size = sizeof(cp);
                        cp.type = C2S_CHAT;
                        strncpy_s(cp.message, sizeof(cp.message), g_chat_buffer.c_str(), _TRUNCATE);
                        cp.message[MAX_CHAT_MSG_LEN - 1] = '\0';
                        send_packet(&cp);
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