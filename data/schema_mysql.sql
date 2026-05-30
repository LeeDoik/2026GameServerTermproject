-- Aetheria Online — MySQL 스키마 (정규화)
-- 적용:  mysql -u root -p < data\schema_mysql.sql
-- (Workbench를 쓰면 이 파일 내용을 새 SQL 탭에 붙여넣고 실행)
--
-- 서버는 PlayerSnapshot(username PK + 스칼라 + inventory[] + quests[])을 이 3개
-- 테이블에 저장/복원한다. 백엔드 구현: Core/Db/MySqlOdbcBackend.cpp

CREATE DATABASE IF NOT EXISTS aetheria CHARACTER SET utf8mb4;

-- 전용 계정 (이미 있으면 무시). 비밀번호는 운영 환경에 맞게 변경 권장.
-- ODBC 호환성을 위해 mysql_native_password 사용 (MySQL 8의 caching_sha2 공개키 핸드셰이크 회피).
CREATE USER IF NOT EXISTS 'aetheria'@'localhost' IDENTIFIED WITH mysql_native_password BY 'aetheria123';
ALTER USER 'aetheria'@'localhost' IDENTIFIED WITH mysql_native_password BY 'aetheria123';
GRANT ALL PRIVILEGES ON aetheria.* TO 'aetheria'@'localhost';
FLUSH PRIVILEGES;

USE aetheria;

-- 플레이어 스칼라 필드 (max_hp는 방어구 보너스 제외한 base 값)
CREATE TABLE IF NOT EXISTS players (
  username           VARCHAR(20)      NOT NULL,
  hp                 INT              NOT NULL,
  max_hp             INT              NOT NULL,
  exp                BIGINT UNSIGNED  NOT NULL,
  level              TINYINT UNSIGNED NOT NULL,
  x                  SMALLINT         NOT NULL,
  y                  SMALLINT         NOT NULL,
  equipped_weapon_id INT              NOT NULL DEFAULT -1,
  equipped_armor_id  INT              NOT NULL DEFAULT -1,
  PRIMARY KEY (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 인벤토리 슬롯 (가변 목록). slot = 인벤 배열 인덱스(순서 보존)
CREATE TABLE IF NOT EXISTS player_inventory (
  username VARCHAR(20) NOT NULL,
  slot     INT         NOT NULL,
  item_id  INT         NOT NULL,
  qty      INT         NOT NULL,
  PRIMARY KEY (username, slot),
  CONSTRAINT fk_inv_user FOREIGN KEY (username)
    REFERENCES players(username) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 퀘스트 진행 (가변 목록). state: 0=active, 1=completed
CREATE TABLE IF NOT EXISTS player_quests (
  username   VARCHAR(20) NOT NULL,
  quest_id   INT         NOT NULL,
  kill_count INT         NOT NULL,
  state      INT         NOT NULL,
  PRIMARY KEY (username, quest_id),
  CONSTRAINT fk_quest_user FOREIGN KEY (username)
    REFERENCES players(username) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
