#pragma once

// Stage 6.3: DB 백엔드 추상 인터페이스.
// 워커 스레드 내부에서만 동기 호출됨. 게임 스레드는 DbWorker를 통한 비동기 큐로만 접근.
// 첫 구현: JsonFileBackend. 추후 SQL Server (ODBC) 백엔드로 교체 가능.

#include "DbTypes.h"

class IDbBackend {
public:
    virtual ~IDbBackend() = default;

    // username으로 조회. 데이터가 있으면 out에 채우고 exists=true 반환.
    // 데이터가 없으면 out.username만 채우고 exists=false. 백엔드 오류 시 ok=false.
    struct LoadResult {
        bool ok = false;
        bool exists = false;
    };
    virtual LoadResult Load(const std::string& username, PlayerSnapshot& out) = 0;

    // 스냅샷 전체 저장 (upsert). 성공 여부 반환.
    virtual bool Save(const PlayerSnapshot& snap) = 0;
};
