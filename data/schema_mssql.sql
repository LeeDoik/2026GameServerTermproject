-- Aetheria Online — SQL Server(MS SQL) 스키마 (정규화)
-- 적용:
--   1) 혼합 모드 인증 + TCP/IP 활성화된 SQL Server 필요 (원격 접속 시 방화벽 1433 개방)
--   2) 관리자(sa 또는 Windows 관리자) 권한으로 실행:
--        sqlcmd -S localhost -U sa -P <sa_pw> -i data\schema_mssql.sql
--      또는 SSMS에서 이 파일을 열고 실행(F5). (GO 배치 구분자 때문에 SSMS 권장)
--
-- 서버는 PlayerSnapshot(username PK + 스칼라 + inventory[] + quests[])을 이 3개
-- 테이블에 저장/복원한다. 백엔드 구현: Core/Db/SqlServerOdbcBackend.cpp

-- 1) DB 생성 (master 컨텍스트에서 실행)
IF DB_ID('aetheria') IS NULL
    CREATE DATABASE aetheria;
GO

-- 2) 전용 SQL 로그인 (서버 수준). 혼합 모드(Mixed Mode) 인증 필요.
--    CHECK_POLICY=OFF: Windows 암호 복잡성 정책 우회(시연/과제용 약한 비밀번호 허용).
IF NOT EXISTS (SELECT 1 FROM sys.server_principals WHERE name = 'aetheria')
    CREATE LOGIN aetheria WITH PASSWORD = 'aetheria123', CHECK_POLICY = OFF;
GO

USE aetheria;
GO

-- 3) DB 사용자 매핑 + 권한 (db_owner = 모든 권한)
IF NOT EXISTS (SELECT 1 FROM sys.database_principals WHERE name = 'aetheria')
    CREATE USER aetheria FOR LOGIN aetheria;
GO
ALTER ROLE db_owner ADD MEMBER aetheria;
GO

-- 4) 플레이어 스칼라 필드 (max_hp는 방어구 보너스 제외한 base 값)
--    타입 매핑(MySQL→SQL Server): BIGINT UNSIGNED→BIGINT, TINYINT UNSIGNED→TINYINT(0~255 부호無),
--    SMALLINT/INT/VARCHAR(20) 동일. ENGINE/CHARSET 절은 SQL Server에 없음.
IF OBJECT_ID('dbo.players', 'U') IS NULL
CREATE TABLE dbo.players (
    username           VARCHAR(20)  NOT NULL,
    hp                 INT          NOT NULL,
    max_hp             INT          NOT NULL,
    exp                BIGINT       NOT NULL,
    level              TINYINT      NOT NULL,
    x                  SMALLINT     NOT NULL,
    y                  SMALLINT     NOT NULL,
    direction          TINYINT      NOT NULL DEFAULT 0,
    equipped_weapon_id INT          NOT NULL DEFAULT -1,
    equipped_armor_id  INT          NOT NULL DEFAULT -1,
    CONSTRAINT pk_players PRIMARY KEY (username)
);
GO

-- 기존 DB에 direction 컬럼 추가 (이미 있으면 무시). MySQL의 information_schema+PREPARE 동적SQL
-- 대신 T-SQL은 COL_LENGTH로 존재 여부를 확인한다(멱등).
IF COL_LENGTH('dbo.players', 'direction') IS NULL
    ALTER TABLE dbo.players ADD direction TINYINT NOT NULL DEFAULT 0;
GO

-- 5) 인벤토리 슬롯 (가변 목록). slot = 인벤 배열 인덱스(순서 보존)
IF OBJECT_ID('dbo.player_inventory', 'U') IS NULL
CREATE TABLE dbo.player_inventory (
    username VARCHAR(20) NOT NULL,
    slot     INT         NOT NULL,
    item_id  INT         NOT NULL,
    qty      INT         NOT NULL,
    CONSTRAINT pk_player_inventory PRIMARY KEY (username, slot),
    CONSTRAINT fk_inv_user FOREIGN KEY (username)
        REFERENCES dbo.players(username) ON DELETE CASCADE
);
GO

-- 6) 퀘스트 진행 (가변 목록). state: 0=active, 1=completed
IF OBJECT_ID('dbo.player_quests', 'U') IS NULL
CREATE TABLE dbo.player_quests (
    username   VARCHAR(20) NOT NULL,
    quest_id   INT         NOT NULL,
    kill_count INT         NOT NULL,
    state      INT         NOT NULL,
    CONSTRAINT pk_player_quests PRIMARY KEY (username, quest_id),
    CONSTRAINT fk_quest_user FOREIGN KEY (username)
        REFERENCES dbo.players(username) ON DELETE CASCADE
);
GO
