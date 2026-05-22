#pragma once

constexpr short PORT = 3500;
constexpr int WORLD_WIDTH = 2000;
constexpr int WORLD_HEIGHT = 2000;
constexpr int MAX_PLAYERS = 10000;
constexpr int NUM_NPCS = 200000;
constexpr int NPC_ID_START = 1000000;
constexpr int NPC_MOVE_INTERVAL = 1000; // in milliseconds
constexpr int MAX_NAME_LEN = 20;
constexpr int MAX_CHAT_MSG_LEN = 200;

enum PACKET_TYPE {
	C2S_LOGIN,			// Client to Server: Login request
	// ����� �̸��� ������ �α��� ��û ��Ŷ	
	C2S_MOVE,			// Client to Server: Move request
	// �̵� ����� �̵� �ð��� ������ �̵� ��û ��Ŷ
	C2S_CHAT,			// Client to Server: Chat message
	// ä�� �޽����� ������ ä�� ��û ��Ŷ
	C2S_ATTACK,			// Client to Server: Attack request
	// ���� ��û ��Ŷ (4 ���� ���� ����)
	C2S_TELEPORT,		// Client to Server: Teleport request
	// �ڷ���Ʈ ��û ��Ŷ (������ ��ǥ ����)
	// STRESS TEST������ �߰��� ��Ŷ�Դϴ�. ���� ������ ������ ���� ����.
	C2S_LOGOUT,			// Client to Server: Logout request

	S2C_LOGIN_RESULT,	//	Server to Client: Login result
	// �α��� ��� ��Ŷ (���� ���ο� �޽��� ����)
	S2C_AVATAR_INFO,	//	Server to Client: Avatar information
	S2C_ADD_OBJECT,		//	Server to Client: Add player or NPC		
	S2C_REMOVE_OBJECT,	//	Server to Client: Remove player or NPC
	S2C_MOVE_OBJECT,	//	Server to Client: Move player or NPC
	S2C_CHAT_MESSAGE,	//	Server to Client: Chat message
	S2C_STATUS_CHANGE,	//	Server to Client: Update player or NPC status (e.g., health, buffs)
	S2C_ATTACK_ANIM,	//	Server to Client: 공격자가 공격 모션을 시작했음 알림 (브로드캐스트)
	S2C_DAMAGE,			//	Server to Client: 피격 통지 (공격자/대상/데미지/대상 새 HP) — 클라가 혈흔 이펙트 + HP 갱신
	S2C_DEATH,			//	Server to Client: 엔티티 사망 (브로드캐스트)
	S2C_RESPAWN,		//	Server to Client: 엔티티 리스폰 (브로드캐스트)
	S2C_LEVEL_UP,		//	Server to Client: 레벨업 알림 (브로드캐스트) — 클라가 burst 이펙트
};

#pragma pack(push, 1) // Ensure no padding between struct members
struct C2S_Login {
	unsigned char size;
	PACKET_TYPE   type;
	char username[MAX_NAME_LEN];
};

struct C2S_Move {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct C2S_Chat {
	unsigned char size;
	PACKET_TYPE   type;
	char message[MAX_CHAT_MSG_LEN];
};

struct C2S_Attack {
	unsigned char size;
	PACKET_TYPE   type;
};

struct C2S_Teleport {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
};

struct C2S_Logout {
	unsigned char size;
	PACKET_TYPE   type;
};

struct S2C_LoginResult {
	unsigned char size;
	PACKET_TYPE   type;
	bool success;
	char message[50];
};

struct S2C_AvatarInfo {
	unsigned char size;
	PACKET_TYPE   type;
	int playerId;
	int visualId; // for future use (different visual appearances)
	short x;
	short y;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_AddObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int visual_id; // for future use (different visual appearances)
	char obj_name[MAX_NAME_LEN];
	short x;
	short y;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_RemoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
};

struct S2C_MoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct S2C_ChatMessage {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	char message[MAX_CHAT_MSG_LEN];
};

struct S2C_StatusChange {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

// 공격 모션 시작 (브로드캐스트). 클라가 워리어 attack 시트를 direction 방향으로 재생.
struct S2C_AttackAnim {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	unsigned char direction; // 0=Down, 1=Left, 2=Right, 3=Up
};

// 피격 통지. 클라가 target 위치에 혈흔 이펙트 + target의 HP HUD 갱신.
struct S2C_Damage {
	unsigned char size;
	PACKET_TYPE   type;
	int attacker_id;
	int target_id;
	int damage;
	int new_hp;       // target의 새 HP (이전 HP - damage, 0 이상)
	short target_x;
	short target_y;
};

// 엔티티 사망. 클라가 사망 위치에 soul 이펙트.
struct S2C_Death {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	short death_x;
	short death_y;
};

// 엔티티 리스폰. 클라가 리스폰 위치에 pillar 이펙트.
struct S2C_Respawn {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	short respawn_x;
	short respawn_y;
	int hp;
	int max_hp;
};

// 레벨업. 클라가 burst 이펙트 + 자기 자신이면 HUD 갱신.
struct S2C_LevelUp {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	unsigned char new_level;
	int new_max_hp;
};

#pragma pack(pop) // Restore default packing
