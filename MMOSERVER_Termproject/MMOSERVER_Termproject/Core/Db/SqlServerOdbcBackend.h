#pragma once

// SQL Server(ODBC) DB 백엔드. IDbBackend 구현.
// 정규화 3테이블(players / player_inventory / player_quests)에 PlayerSnapshot을 저장/복원.
// DbWorker의 단일 스레드에서만 호출되므로 연결 1개를 독점 — 내부 락 불필요.
// 헤더에서 sql.h 노출을 피하려고 ODBC 핸들은 void*로 보관 (LuaVM과 동일한 은닉 패턴).

#include "IDbBackend.h"
#include <string>

class SqlServerOdbcBackend : public IDbBackend {
public:
    explicit SqlServerOdbcBackend(const std::string& conn_str);
    ~SqlServerOdbcBackend() override;

    SqlServerOdbcBackend(const SqlServerOdbcBackend&) = delete;
    SqlServerOdbcBackend& operator=(const SqlServerOdbcBackend&) = delete;

    // 연결 성공 여부. main()이 false면 JSON 백엔드로 폴백.
    bool Connected() const { return connected_; }

    LoadResult Load(const std::string& username, PlayerSnapshot& out) override;
    bool Save(const PlayerSnapshot& snap) override;

private:
    void* env_ = nullptr;       // SQLHENV
    void* dbc_ = nullptr;       // SQLHDBC
    bool  connected_ = false;
};
