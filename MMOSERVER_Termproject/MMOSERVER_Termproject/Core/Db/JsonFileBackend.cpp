#define _CRT_SECURE_NO_WARNINGS
#include "JsonFileBackend.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <direct.h>
#include <sys/stat.h>
#include <string>
#include <utility>

namespace {

// username 안전화: 영숫자 + 언더스코어만 허용. 그 외는 '_'로 치환.
// 파일 시스템 traversal 방지 + Windows 예약 문자 회피.
std::string SafeUsername(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        }
        else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "_";
    return out;
}

void EnsureDir(const std::string& path) {
    struct _stat st;
    if (_stat(path.c_str(), &st) == 0) return;
    _mkdir(path.c_str());
}

// 단순 키 검색: "\"key\":" 패턴 위치를 찾아 그 뒤의 값 시작을 가리킴.
// 못 찾으면 nullptr.
const char* FindKey(const char* json, const char* key) {
    std::string pat = "\"";
    pat += key;
    pat += "\":";
    const char* p = std::strstr(json, pat.c_str());
    if (!p) return nullptr;
    p += pat.size();
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

bool ReadString(const char* p, std::string& out) {
    if (*p != '"') return false;
    ++p;
    out.clear();
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) { out.push_back(p[1]); p += 2; }
        else { out.push_back(*p++); }
    }
    return *p == '"';
}

bool ReadInt(const char* p, long long& out) {
    char* end = nullptr;
    out = std::strtoll(p, &end, 10);
    return end != p;
}

// "inventory":[[id,qty],[id,qty],...] 형태의 정수쌍 배열을 파싱.
void ReadIntPairArray(const char* p, std::vector<std::pair<int, int>>& out) {
    out.clear();
    if (*p != '[') return;
    ++p;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (*p == ']' || *p == '\0') break;
        if (*p != '[') break;  // 형식 오류 — 중단
        ++p;
        char* end = nullptr;
        long a = std::strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        long b = std::strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        while (*p && *p != ']') ++p;  // inner ']'까지 스킵
        if (*p == ']') ++p;           // inner ']' 소비
        out.emplace_back(static_cast<int>(a), static_cast<int>(b));
        if (out.size() > 64) break;   // 안전 상한
    }
}

// "quests":[[id,cnt,state],...] 형태의 정수3쌍 배열을 파싱. (Stage 9)
void ReadIntTripleArray(const char* p, std::vector<std::array<int, 3>>& out) {
    out.clear();
    if (*p != '[') return;
    ++p;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (*p == ']' || *p == '\0') break;
        if (*p != '[') break;  // 형식 오류 — 중단
        ++p;
        long vals[3] = { 0, 0, 0 };
        bool bad = false;
        for (int i = 0; i < 3; ++i) {
            char* end = nullptr;
            vals[i] = std::strtol(p, &end, 10);
            if (end == p) { bad = true; break; }
            p = end;
            while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        }
        if (bad) break;
        while (*p && *p != ']') ++p;  // inner ']'까지 스킵
        if (*p == ']') ++p;           // inner ']' 소비
        out.push_back({ static_cast<int>(vals[0]), static_cast<int>(vals[1]), static_cast<int>(vals[2]) });
        if (out.size() > 256) break;  // 안전 상한
    }
}

} // anon

JsonFileBackend::JsonFileBackend(std::string root_dir) : root_dir_(std::move(root_dir)) {
    EnsureDir(root_dir_);
}

std::string JsonFileBackend::PathFor(const std::string& username) const {
    std::string safe = SafeUsername(username);
    std::string p = root_dir_;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
    p += safe;
    p += ".json";
    return p;
}

IDbBackend::LoadResult JsonFileBackend::Load(const std::string& username, PlayerSnapshot& out) {
    LoadResult res;
    out.username = username;

    std::string path = PathFor(username);
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        // 파일 없음 = 신규 캐릭터 (백엔드 호출 자체는 성공으로 간주)
        res.ok = true;
        res.exists = false;
        return res;
    }

    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024) {
        std::fclose(fp);
        res.ok = false;
        return res;
    }
    std::string buf(static_cast<size_t>(size), '\0');
    size_t read = std::fread(buf.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (read != static_cast<size_t>(size)) {
        res.ok = false;
        return res;
    }

    const char* json = buf.c_str();

    // username
    if (const char* p = FindKey(json, "username")) {
        std::string s;
        if (ReadString(p, s)) out.username = std::move(s);
    }

    long long v = 0;
    if (const char* p = FindKey(json, "hp"))      { if (ReadInt(p, v)) out.hp = static_cast<int>(v); }
    if (const char* p = FindKey(json, "max_hp"))  { if (ReadInt(p, v)) out.max_hp = static_cast<int>(v); }
    if (const char* p = FindKey(json, "exp"))     { if (ReadInt(p, v)) out.exp = static_cast<unsigned long long>(v); }
    if (const char* p = FindKey(json, "level"))   { if (ReadInt(p, v)) out.level = static_cast<unsigned char>(v); }
    if (const char* p = FindKey(json, "x"))       { if (ReadInt(p, v)) out.x = static_cast<short>(v); }
    if (const char* p = FindKey(json, "y"))       { if (ReadInt(p, v)) out.y = static_cast<short>(v); }

    // Stage 8: 아이템 (키 없으면 기본값 — 구버전 JSON 호환)
    if (const char* p = FindKey(json, "weapon"))    { if (ReadInt(p, v)) out.equipped_weapon_id = static_cast<int>(v); }
    if (const char* p = FindKey(json, "armor"))     { if (ReadInt(p, v)) out.equipped_armor_id  = static_cast<int>(v); }
    if (const char* p = FindKey(json, "inventory")) { ReadIntPairArray(p, out.inventory); }

    // Stage 9: 퀘스트 (키 없으면 빈 목록 — 구버전 JSON 호환)
    if (const char* p = FindKey(json, "quests")) { ReadIntTripleArray(p, out.quests); }

    res.ok = true;
    res.exists = true;
    return res;
}

bool JsonFileBackend::Save(const PlayerSnapshot& snap) {
    std::string path = PathFor(snap.username);
    // 임시 파일에 먼저 쓰고 rename — 부분 쓰기로 인한 손상 방지 (atomic-ish)
    std::string tmp = path + ".tmp";

    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) return false;

    // Stage 8: inventory 배열 직렬화 → [[id,qty],...]
    std::string inv = "[";
    for (size_t i = 0; i < snap.inventory.size(); ++i) {
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "%s[%d,%d]",
            (i ? "," : ""), snap.inventory[i].first, snap.inventory[i].second);
        inv += tmp;
    }
    inv += "]";

    // Stage 9: quests 배열 직렬화 → [[id,cnt,state],...]
    std::string qs = "[";
    for (size_t i = 0; i < snap.quests.size(); ++i) {
        char qb[64];
        std::snprintf(qb, sizeof(qb), "%s[%d,%d,%d]",
            (i ? "," : ""), snap.quests[i][0], snap.quests[i][1], snap.quests[i][2]);
        qs += qb;
    }
    qs += "]";

    // 외부 라이브러리 없이 one-liner JSON. username은 안전화된 키 그대로 사용.
    int n = std::fprintf(
        fp,
        "{\"username\":\"%s\",\"hp\":%d,\"max_hp\":%d,\"exp\":%llu,\"level\":%u,\"x\":%d,\"y\":%d,"
        "\"inventory\":%s,\"weapon\":%d,\"armor\":%d,\"quests\":%s}\n",
        SafeUsername(snap.username).c_str(),
        snap.hp,
        snap.max_hp,
        static_cast<unsigned long long>(snap.exp),
        static_cast<unsigned int>(snap.level),
        static_cast<int>(snap.x),
        static_cast<int>(snap.y),
        inv.c_str(),
        snap.equipped_weapon_id,
        snap.equipped_armor_id,
        qs.c_str());
    std::fclose(fp);

    if (n <= 0) {
        std::remove(tmp.c_str());
        return false;
    }

    // Windows: 기존 파일 있으면 rename 실패. 먼저 제거.
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}
