#define _CRT_SECURE_NO_WARNINGS
#include "Item.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <iostream>

std::unordered_map<int, ItemDef> g_item_defs;

const ItemDef* GetItemDef(int item_id) {
    auto it = g_item_defs.find(item_id);
    return (it == g_item_defs.end()) ? nullptr : &it->second;
}

int LoadItemDefs(const char* path) {
    FILE* fp = std::fopen(path, "r");
    if (!fp) return -1;

    int loaded = 0;
    char line[256];
    int line_no = 0;
    while (std::fgets(line, sizeof(line), fp)) {
        ++line_no;
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        int id = 0, type = 0, value = 0, stack_max = 1, drop_weight = 0;
        char name_s[64] = { 0 };
        int matched = std::sscanf(p, "%d %63s %d %d %d %d",
            &id, name_s, &type, &value, &stack_max, &drop_weight);
        if (matched != 6) {
            std::cerr << "[Item] Line " << line_no << " parse error: " << line;
            continue;
        }
        if (type < 0 || type > 2) {
            std::cerr << "[Item] Line " << line_no << " bad type: " << type << std::endl;
            continue;
        }

        ItemDef def;
        def.id = id;
        std::snprintf(def.name, sizeof(def.name), "%s", name_s);
        def.type = static_cast<ItemType>(type);
        def.value = value;
        def.stack_max = (stack_max < 1) ? 1 : stack_max;
        def.drop_weight = (drop_weight < 0) ? 0 : drop_weight;
        g_item_defs[id] = def;
        ++loaded;
    }
    std::fclose(fp);
    return loaded;
}

namespace {

// 워커 스레드마다 독립 RNG (cppreference 표준 idiom). 드롭은 결정성 불필요.
std::mt19937& Rng() {
    thread_local std::mt19937 rng{ std::random_device{}() };
    return rng;
}

int RollPercent() {
    std::uniform_int_distribution<int> d(0, 99);
    return d(Rng());
}

// drop_weight>0 + 타입 필터에 해당하는 정의 중 가중 랜덤 1개 (없으면 -1)
template <typename Pred>
int WeightedPick(Pred pred) {
    int total = 0;
    for (const auto& kv : g_item_defs) {
        const ItemDef& d = kv.second;
        if (d.drop_weight > 0 && pred(d)) total += d.drop_weight;
    }
    if (total <= 0) return -1;
    std::uniform_int_distribution<int> dist(0, total - 1);
    int r = dist(Rng());
    for (const auto& kv : g_item_defs) {
        const ItemDef& d = kv.second;
        if (d.drop_weight > 0 && pred(d)) {
            r -= d.drop_weight;
            if (r < 0) return d.id;
        }
    }
    return -1;
}

} // anon

bool RollShouldDrop(NpcType type, NpcMoveMode /*mode*/) {
    if (type == NpcType::Boss) return true;
    int chance = (type == NpcType::Agro) ? 50 : 35;
    return RollPercent() < chance;
}

std::vector<int> RollDropItems(NpcType type, int /*level*/) {
    std::vector<int> out;
    if (type == NpcType::Boss) {
        // 보스: 무기 + 방어구 + 고급 포션 보장 (없으면 일반 가중 랜덤으로 대체)
        int w = WeightedPick([](const ItemDef& d) { return d.type == ItemType::Weapon; });
        int a = WeightedPick([](const ItemDef& d) { return d.type == ItemType::Armor; });
        if (w >= 0) out.push_back(w);
        if (a >= 0) out.push_back(a);
        // 포션: GreatHealthPotion(2) 우선, 없으면 아무 소모품
        if (GetItemDef(2)) out.push_back(2);
        else {
            int c = WeightedPick([](const ItemDef& d) { return d.type == ItemType::Consumable; });
            if (c >= 0) out.push_back(c);
        }
        if (out.empty()) {
            int any = WeightedPick([](const ItemDef&) { return true; });
            if (any >= 0) out.push_back(any);
        }
        return out;
    }

    // 일반/Agro: 전체 가중 랜덤 1개
    int pick = WeightedPick([](const ItemDef&) { return true; });
    if (pick >= 0) out.push_back(pick);
    return out;
}
