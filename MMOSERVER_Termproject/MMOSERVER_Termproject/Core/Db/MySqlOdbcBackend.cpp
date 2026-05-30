#define _CRT_SECURE_NO_WARNINGS
#include "MySqlOdbcBackend.h"

#include <windows.h>
#include <sql.h>
#include <sqlext.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "odbc32.lib")

// 모든 DB 호출은 DbWorker의 단일 스레드에서만 일어나므로 핸들 공유 동기화가 필요 없다.

namespace {

inline bool Ok(SQLRETURN r) { return r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO; }

// 진단 레코드를 stderr로 출력 (크래시 없이 로깅만).
void ReportError(const char* ctx, SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR state[6] = { 0 };
    SQLCHAR msg[512] = { 0 };
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    SQLSMALLINT rec = 1;
    while (SQLGetDiagRecA(handle_type, handle, rec, state, &native, msg,
                          static_cast<SQLSMALLINT>(sizeof(msg)), &msg_len) == SQL_SUCCESS) {
        std::fprintf(stderr, "[Db] ODBC error (%s) [%s] %s\n", ctx,
                     reinterpret_cast<char*>(state), reinterpret_cast<char*>(msg));
        ++rec;
    }
}

// username을 input VARCHAR 파라미터로 바인딩 (버퍼는 호출자가 stable하게 보유).
SQLRETURN BindUser(SQLHSTMT st, SQLUSMALLINT pnum, char* buf, SQLLEN* ind) {
    return SQLBindParameter(st, pnum, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                            20, 0, buf, 0, ind);
}

// int input 파라미터 바인딩.
SQLRETURN BindInt(SQLHSTMT st, SQLUSMALLINT pnum, SQLSMALLINT sql_type, int* val) {
    return SQLBindParameter(st, pnum, SQL_PARAM_INPUT, SQL_C_SLONG, sql_type,
                            0, 0, val, 0, nullptr);
}

} // namespace

MySqlOdbcBackend::MySqlOdbcBackend(const std::string& conn_str) {
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;

    if (!Ok(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        std::fprintf(stderr, "[Db] SQLAllocHandle(ENV) failed.\n");
        return;
    }
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);

    if (!Ok(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc))) {
        ReportError("AllocDbc", SQL_HANDLE_ENV, env);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return;
    }

    SQLCHAR out_str[1024] = { 0 };
    SQLSMALLINT out_len = 0;
    SQLRETURN r = SQLDriverConnectA(
        dbc, nullptr,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(conn_str.c_str())), SQL_NTS,
        out_str, static_cast<SQLSMALLINT>(sizeof(out_str)), &out_len,
        SQL_DRIVER_NOPROMPT);

    if (!Ok(r)) {
        ReportError("DriverConnect", SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return;
    }

    env_ = env;
    dbc_ = dbc;
    connected_ = true;
}

MySqlOdbcBackend::~MySqlOdbcBackend() {
    if (dbc_) {
        SQLDisconnect(static_cast<SQLHDBC>(dbc_));
        SQLFreeHandle(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_));
    }
    if (env_) {
        SQLFreeHandle(SQL_HANDLE_ENV, static_cast<SQLHENV>(env_));
    }
}

IDbBackend::LoadResult MySqlOdbcBackend::Load(const std::string& username, PlayerSnapshot& out) {
    LoadResult res;
    out.username = username;
    if (!connected_) return res;  // ok=false

    SQLHDBC dbc = static_cast<SQLHDBC>(dbc_);

    char user[64];
    std::strncpy(user, username.c_str(), sizeof(user) - 1);
    user[sizeof(user) - 1] = '\0';
    SQLLEN user_ind = SQL_NTS;

    // --- 1) players 스칼라 행 ---
    {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (!Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            ReportError("Load/AllocStmt", SQL_HANDLE_DBC, dbc);
            return res;
        }
        const char* sql =
            "SELECT hp,max_hp,exp,level,x,y,equipped_weapon_id,equipped_armor_id "
            "FROM players WHERE username=?";
        BindUser(st, 1, user, &user_ind);
        if (!Ok(SQLExecDirectA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS))) {
            ReportError("Load/players", SQL_HANDLE_STMT, st);
            SQLFreeHandle(SQL_HANDLE_STMT, st);
            return res;
        }

        SQLRETURN fr = SQLFetch(st);
        if (fr == SQL_NO_DATA) {
            // 행 없음 = 신규 캐릭 (백엔드 호출 자체는 성공)
            SQLFreeHandle(SQL_HANDLE_STMT, st);
            res.ok = true;
            res.exists = false;
            return res;
        }
        if (!Ok(fr)) {
            ReportError("Load/fetch", SQL_HANDLE_STMT, st);
            SQLFreeHandle(SQL_HANDLE_STMT, st);
            return res;
        }

        long hp = 100, max_hp = 100, level = 1, x = 0, y = 0, wpn = -1, arm = -1;
        unsigned long long exp = 0;
        SQLLEN ind = 0;
        SQLGetData(st, 1, SQL_C_SLONG, &hp, 0, &ind);
        SQLGetData(st, 2, SQL_C_SLONG, &max_hp, 0, &ind);
        SQLGetData(st, 3, SQL_C_UBIGINT, &exp, 0, &ind);
        SQLGetData(st, 4, SQL_C_SLONG, &level, 0, &ind);
        SQLGetData(st, 5, SQL_C_SLONG, &x, 0, &ind);
        SQLGetData(st, 6, SQL_C_SLONG, &y, 0, &ind);
        SQLGetData(st, 7, SQL_C_SLONG, &wpn, 0, &ind);
        SQLGetData(st, 8, SQL_C_SLONG, &arm, 0, &ind);

        out.hp = static_cast<int>(hp);
        out.max_hp = static_cast<int>(max_hp);
        out.exp = exp;
        out.level = static_cast<unsigned char>(level);
        out.x = static_cast<short>(x);
        out.y = static_cast<short>(y);
        out.equipped_weapon_id = static_cast<int>(wpn);
        out.equipped_armor_id = static_cast<int>(arm);

        SQLFreeHandle(SQL_HANDLE_STMT, st);
    }

    // --- 2) player_inventory (slot 순) ---
    {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* sql =
                "SELECT item_id,qty FROM player_inventory WHERE username=? ORDER BY slot";
            BindUser(st, 1, user, &user_ind);
            if (Ok(SQLExecDirectA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS))) {
                out.inventory.clear();
                while (SQLFetch(st) == SQL_SUCCESS) {
                    long item_id = 0, qty = 0;
                    SQLLEN ind = 0;
                    SQLGetData(st, 1, SQL_C_SLONG, &item_id, 0, &ind);
                    SQLGetData(st, 2, SQL_C_SLONG, &qty, 0, &ind);
                    out.inventory.emplace_back(static_cast<int>(item_id), static_cast<int>(qty));
                }
            } else {
                ReportError("Load/inventory", SQL_HANDLE_STMT, st);
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        }
    }

    // --- 3) player_quests ---
    {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* sql =
                "SELECT quest_id,kill_count,state FROM player_quests WHERE username=?";
            BindUser(st, 1, user, &user_ind);
            if (Ok(SQLExecDirectA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS))) {
                out.quests.clear();
                while (SQLFetch(st) == SQL_SUCCESS) {
                    long qid = 0, cnt = 0, state = 0;
                    SQLLEN ind = 0;
                    SQLGetData(st, 1, SQL_C_SLONG, &qid, 0, &ind);
                    SQLGetData(st, 2, SQL_C_SLONG, &cnt, 0, &ind);
                    SQLGetData(st, 3, SQL_C_SLONG, &state, 0, &ind);
                    out.quests.push_back({ static_cast<int>(qid),
                                           static_cast<int>(cnt),
                                           static_cast<int>(state) });
                }
            } else {
                ReportError("Load/quests", SQL_HANDLE_STMT, st);
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        }
    }

    res.ok = true;
    res.exists = true;
    return res;
}

bool MySqlOdbcBackend::Save(const PlayerSnapshot& snap) {
    if (!connected_) return false;
    SQLHDBC dbc = static_cast<SQLHDBC>(dbc_);

    // 트랜잭션 시작 (autocommit off)
    SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
                      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
    bool ok = true;

    // 파라미터 바인딩용 stable 버퍼
    char user[64];
    std::strncpy(user, snap.username.c_str(), sizeof(user) - 1);
    user[sizeof(user) - 1] = '\0';
    SQLLEN user_ind = SQL_NTS;

    int hp = snap.hp, max_hp = snap.max_hp, level = snap.level;
    int x = snap.x, y = snap.y;
    int wpn = snap.equipped_weapon_id, arm = snap.equipped_armor_id;
    unsigned long long exp = snap.exp;

    // --- 1) players upsert ---
    if (ok) {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* sql =
                "INSERT INTO players "
                "(username,hp,max_hp,exp,level,x,y,equipped_weapon_id,equipped_armor_id) "
                "VALUES (?,?,?,?,?,?,?,?,?) "
                "ON DUPLICATE KEY UPDATE "
                "hp=VALUES(hp),max_hp=VALUES(max_hp),exp=VALUES(exp),level=VALUES(level),"
                "x=VALUES(x),y=VALUES(y),"
                "equipped_weapon_id=VALUES(equipped_weapon_id),"
                "equipped_armor_id=VALUES(equipped_armor_id)";
            BindUser(st, 1, user, &user_ind);
            BindInt(st, 2, SQL_INTEGER, &hp);
            BindInt(st, 3, SQL_INTEGER, &max_hp);
            SQLBindParameter(st, 4, SQL_PARAM_INPUT, SQL_C_UBIGINT, SQL_BIGINT, 0, 0, &exp, 0, nullptr);
            BindInt(st, 5, SQL_TINYINT, &level);
            BindInt(st, 6, SQL_SMALLINT, &x);
            BindInt(st, 7, SQL_SMALLINT, &y);
            BindInt(st, 8, SQL_INTEGER, &wpn);
            BindInt(st, 9, SQL_INTEGER, &arm);
            if (!Ok(SQLExecDirectA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS))) {
                ReportError("Save/players", SQL_HANDLE_STMT, st);
                ok = false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        } else {
            ReportError("Save/AllocStmt", SQL_HANDLE_DBC, dbc);
            ok = false;
        }
    }

    // --- 2) player_inventory: 전삭제 후 재삽입 ---
    if (ok) {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* del = "DELETE FROM player_inventory WHERE username=?";
            BindUser(st, 1, user, &user_ind);
            if (!Ok(SQLExecDirectA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(del)), SQL_NTS))) {
                ReportError("Save/inv-del", SQL_HANDLE_STMT, st);
                ok = false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        } else { ok = false; }
    }
    if (ok && !snap.inventory.empty()) {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* ins =
                "INSERT INTO player_inventory (username,slot,item_id,qty) VALUES (?,?,?,?)";
            int slot = 0, item_id = 0, qty = 0;
            BindUser(st, 1, user, &user_ind);
            BindInt(st, 2, SQL_INTEGER, &slot);
            BindInt(st, 3, SQL_INTEGER, &item_id);
            BindInt(st, 4, SQL_INTEGER, &qty);
            if (Ok(SQLPrepareA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(ins)), SQL_NTS))) {
                for (size_t i = 0; i < snap.inventory.size() && ok; ++i) {
                    slot = static_cast<int>(i);
                    item_id = snap.inventory[i].first;
                    qty = snap.inventory[i].second;
                    if (!Ok(SQLExecute(st))) {
                        ReportError("Save/inv-ins", SQL_HANDLE_STMT, st);
                        ok = false;
                    }
                }
            } else {
                ReportError("Save/inv-prep", SQL_HANDLE_STMT, st);
                ok = false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        } else { ok = false; }
    }

    // --- 3) player_quests: 전삭제 후 재삽입 ---
    if (ok) {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* del = "DELETE FROM player_quests WHERE username=?";
            BindUser(st, 1, user, &user_ind);
            if (!Ok(SQLExecDirectA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(del)), SQL_NTS))) {
                ReportError("Save/quest-del", SQL_HANDLE_STMT, st);
                ok = false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        } else { ok = false; }
    }
    if (ok && !snap.quests.empty()) {
        SQLHSTMT st = SQL_NULL_HSTMT;
        if (Ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
            const char* ins =
                "INSERT INTO player_quests (username,quest_id,kill_count,state) VALUES (?,?,?,?)";
            int qid = 0, cnt = 0, state = 0;
            BindUser(st, 1, user, &user_ind);
            BindInt(st, 2, SQL_INTEGER, &qid);
            BindInt(st, 3, SQL_INTEGER, &cnt);
            BindInt(st, 4, SQL_INTEGER, &state);
            if (Ok(SQLPrepareA(st, reinterpret_cast<SQLCHAR*>(const_cast<char*>(ins)), SQL_NTS))) {
                for (size_t i = 0; i < snap.quests.size() && ok; ++i) {
                    qid = snap.quests[i][0];
                    cnt = snap.quests[i][1];
                    state = snap.quests[i][2];
                    if (!Ok(SQLExecute(st))) {
                        ReportError("Save/quest-ins", SQL_HANDLE_STMT, st);
                        ok = false;
                    }
                }
            } else {
                ReportError("Save/quest-prep", SQL_HANDLE_STMT, st);
                ok = false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        } else { ok = false; }
    }

    // 커밋/롤백 후 autocommit 복원
    SQLEndTran(SQL_HANDLE_DBC, dbc, ok ? SQL_COMMIT : SQL_ROLLBACK);
    SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
                      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
    return ok;
}
