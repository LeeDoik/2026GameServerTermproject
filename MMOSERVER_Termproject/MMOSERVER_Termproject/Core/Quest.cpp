#define _CRT_SECURE_NO_WARNINGS
#include "Quest.h"

#include <cstdio>
#include <cstring>
#include <iostream>

std::vector<QuestDef> g_quest_defs;

const QuestDef* GetQuestDef(int quest_id) {
    for (const QuestDef& d : g_quest_defs) {
        if (d.id == quest_id) return &d;
    }
    return nullptr;
}

int LoadQuestDefs(const char* path) {
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

        int id = 0, prereq = -1, giver = 0, count = 1;
        int reward_item = -1, reward_qty = 0;
        unsigned long long reward_exp = 0;
        char species_s[64] = { 0 };
        int matched = std::sscanf(p, "%d %d %d %63s %d %llu %d %d",
            &id, &prereq, &giver, species_s, &count, &reward_exp, &reward_item, &reward_qty);
        if (matched != 8) {
            std::cerr << "[Quest] Line " << line_no << " parse error: " << line;
            continue;
        }

        QuestDef def;
        def.id = id;
        def.prereq_id = prereq;
        def.giver_npc = static_cast<unsigned char>(giver < 0 ? 0 : giver);
        std::snprintf(def.target_species, sizeof(def.target_species), "%s", species_s);
        def.target_count = (count < 1) ? 1 : count;
        def.reward_exp = reward_exp;
        def.reward_item_id = reward_item;
        def.reward_item_qty = (reward_qty < 0) ? 0 : reward_qty;
        g_quest_defs.push_back(def);
        ++loaded;
    }
    std::fclose(fp);
    return loaded;
}

bool QuestSpeciesMatches(const char* species, const char* npc_name) {
    if (!species || !npc_name) return false;
    // npc_name = "<species>_<5자리>". '_' 직전까지가 종족 접두사.
    size_t slen = std::strlen(species);
    if (std::strncmp(species, npc_name, slen) != 0) return false;
    // 접두사 직후가 문자열 끝이거나 '_' 여야 정확히 일치 (Slime이 SlimeKing에 오매칭 방지)
    char after = npc_name[slen];
    return (after == '\0' || after == '_');
}
