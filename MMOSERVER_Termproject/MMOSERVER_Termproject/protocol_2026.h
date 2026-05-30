#pragma once

constexpr short PORT = 3500;
constexpr int WORLD_WIDTH = 2000;
constexpr int WORLD_HEIGHT = 2000;
constexpr int MAX_PLAYERS = 10000;
constexpr int NUM_NPCS = 200000;
constexpr int NPC_ID_START = 1000000;
// NPC 이동 주기는 GameConfig.h의 NPC_TICK_INTERVAL_MS(500ms)가 단일 진실원.
// (구버전 NPC_MOVE_INTERVAL 상수는 미사용이라 제거됨)
constexpr int MAX_NAME_LEN = 20;
constexpr int MAX_CHAT_MSG_LEN = 200;
constexpr int MAX_INVENTORY_SLOTS = 20; // Stage 8: 인벤토리 슬롯 수 (S2C_Inventory 패킷 크기 < 256 보장)

enum PACKET_TYPE {
	C2S_LOGIN,			// Client to Server: Login request
	// 사용자 이름을 포함한 로그인 요청 패킷
	C2S_MOVE,			// Client to Server: Move request
	// 목표 좌표와 이동 시각을 포함한 이동 요청 패킷
	C2S_CHAT,			// Client to Server: Chat message
	// 채팅 메시지를 포함한 채팅 요청 패킷
	C2S_ATTACK,			// Client to Server: Attack request
	// 4방향 근접 공격 요청 패킷
	C2S_TELEPORT,		// Client to Server: Teleport request
	// 텔레포트 요청 패킷 (목적지 좌표 포함)
	// STRESS TEST 전용으로 추가된 패킷. 실제 게임 플레이에는 사용되지 않을 수 있음.
	C2S_LOGOUT,			// Client to Server: Logout request
	C2S_USE_SKILL,		// Client to Server: Skill use request (skill_id: 1=AoE, 2=Line, 3=Heal)
	C2S_PARTY_INVITE,	// Client to Server: 파티 초대 (target_name)
	C2S_PARTY_ACCEPT,	// Client to Server: 파티 초대 수락
	C2S_PARTY_REJECT,	// Client to Server: 파티 초대 거절
	C2S_PARTY_LEAVE,	// Client to Server: 파티 탈퇴

	S2C_LOGIN_RESULT,	//	Server to Client: Login result
	// 로그인 결과 패킷 (성공 여부와 메시지 포함)
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
	S2C_SKILL_EFFECT,	//	Server to Client: 스킬 발동 통지 (브로드캐스트) — 클라가 스킬 이펙트 재생
	S2C_PARTY_INVITED,	//	Server to Client: 파티 초대 수신 알림
	S2C_PARTY_UPDATE,	//	Server to Client: 파티 상태 변경 (0=joined, 1=left, 2=disbanded)

	// Stage 8: 아이템 시스템
	C2S_PICKUP,			// Client to Server: 근처 바닥 아이템 줍기
	C2S_USE_ITEM,		// Client to Server: 인벤 슬롯 아이템 사용 (slot)
	C2S_EQUIP_ITEM,		// Client to Server: 인벤 슬롯 아이템 장착 (slot)
	C2S_UNEQUIP_ITEM,	// Client to Server: 장착 해제 (which: 0=weapon, 1=armor)
	S2C_ITEM_DROP,		// Server to Client: 바닥 아이템 생성 (drop_id/item_id/x/y)
	S2C_ITEM_REMOVE,	// Server to Client: 바닥 아이템 제거 (줍힘/만료)
	S2C_INVENTORY,		// Server to Client: 인벤토리 + 장착 전체 스냅샷 (본인 전용)

	// Stage 9: 퀘스트 시스템
	C2S_QUEST_INTERACT,	// Client to Server: 마을 NPC(장로)와 대화 시도 (npc_index)
	C2S_QUEST_ACTION,	// Client to Server: 대화창에서 수락/보상수령 확정 (quest_id, action)
	S2C_QUEST_DIALOGUE,	// Server to Client: 대화창 표시 지시 (kind)
	S2C_QUEST_UPDATE,	// Server to Client: 퀘스트 상태/진행 동기화
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

struct C2S_UseSkill {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char skill_id; // 1=AoE(3칸반경), 2=Line(방향5칸), 3=Heal(자신HP30%회복)
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

// 스킬 발동 통지. 클라가 skill_id에 따라 이펙트 재생.
// skill_id: 1=AoE(화염원형), 2=Line(관통빔), 3=Heal(골드오라)
// direction: 0=Down 1=Left 2=Right 3=Up (Line 스킬 경로 표시에 사용)
struct S2C_SkillEffect {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;       // 시전자 ID
	unsigned char skill_id;
	unsigned char direction; // 시전자 방향 (Line 빔 진행 방향)
	short x;             // 시전 위치
	short y;
};

// 파티 초대 발송: target 플레이어 이름 포함
struct C2S_PartyInvite {
	unsigned char size;
	PACKET_TYPE   type;
	char target_name[MAX_NAME_LEN];
};

struct C2S_PartyAccept { unsigned char size; PACKET_TYPE type; };
struct C2S_PartyReject { unsigned char size; PACKET_TYPE type; };
struct C2S_PartyLeave  { unsigned char size; PACKET_TYPE type; };

// 초대 수신 알림: 누가 초대했는지 표시
struct S2C_PartyInvited {
	unsigned char size;
	PACKET_TYPE   type;
	int inviter_id;
	char inviter_name[MAX_NAME_LEN];
};

// 파티 상태 변경. event: 0=joined, 1=left, 2=disbanded
struct S2C_PartyUpdate {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char event;
	int member_id;
	char member_name[MAX_NAME_LEN];
};

// === Stage 8: 아이템 패킷 ===
struct C2S_PickUp {
	unsigned char size;
	PACKET_TYPE   type;
};

struct C2S_UseItem {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char slot; // 인벤토리 슬롯 인덱스
};

struct C2S_EquipItem {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char slot; // 인벤토리 슬롯 인덱스
};

struct C2S_UnequipItem {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char which; // 0=weapon, 1=armor
};

// 바닥 아이템 생성. 클라가 (x,y)에 아이템 스프라이트를 그림.
struct S2C_ItemDrop {
	unsigned char size;
	PACKET_TYPE   type;
	int   drop_id;   // 바닥 아이템 고유 ID
	int   item_id;   // 카탈로그 아이템 ID
	short x;
	short y;
};

// 바닥 아이템 제거 (줍힘/만료).
struct S2C_ItemRemove {
	unsigned char size;
	PACKET_TYPE   type;
	int drop_id;
};

struct InvSlotNet {
	int item_id;
	int qty;
};

// 인벤토리 + 장착 전체 스냅샷 (본인에게만). 변경 시마다 전체 재전송.
struct S2C_Inventory {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char count;                    // 사용 중인 슬롯 수 (0..MAX_INVENTORY_SLOTS)
	InvSlotNet    slots[MAX_INVENTORY_SLOTS];
	int           equipped_weapon_id;       // -1 = 없음
	int           equipped_armor_id;        // -1 = 없음
};

// === Stage 9: 퀘스트 패킷 ===
// 마을 NPC(장로)와 대화 시도. 서버가 좌표 근접성을 검증.
struct C2S_QuestInteract {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char npc_index; // 0 = 장로(퀘스트 giver)
};

// 대화창에서의 확정 입력.
struct C2S_QuestAction {
	unsigned char size;
	PACKET_TYPE   type;
	int           quest_id;
	unsigned char action;    // 0=accept, 1=turnin
};

// 서버가 클라에 어떤 대화창을 띄울지 지시.
struct S2C_QuestDialogue {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char npc_index;
	int           quest_id;  // 관련 퀘스트 (kind=None이면 -1)
	unsigned char kind;      // 0=Offer(수락가능) 1=InProgress(진행중) 2=ReadyTurnIn(보상수령) 3=None
};

// 퀘스트 상태/진행 동기화 (수락/킬진행/완료/로그인복원 시).
struct S2C_QuestUpdate {
	unsigned char size;
	PACKET_TYPE   type;
	int           quest_id;
	int           kill_count;
	int           target_count;
	unsigned char state;     // 0=active, 1=completed
};

#pragma pack(pop) // Restore default packing
