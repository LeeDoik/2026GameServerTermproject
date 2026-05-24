#pragma once

// Stage 6.3: 파일 기반 DB 백엔드. 한 유저당 한 JSON 파일 (data/players/<username>.json).
// 외부 JSON 라이브러리 없이 자체 minimal 파서/시리얼라이저 사용 — 6개 고정 필드 한정.

#include "IDbBackend.h"
#include <string>

class JsonFileBackend : public IDbBackend {
public:
    explicit JsonFileBackend(std::string root_dir);

    LoadResult Load(const std::string& username, PlayerSnapshot& out) override;
    bool Save(const PlayerSnapshot& snap) override;

private:
    std::string root_dir_;

    std::string PathFor(const std::string& username) const;
};
