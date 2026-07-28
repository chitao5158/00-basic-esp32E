-- ==============================================================
-- 远程盆栽监控 — 数据库表结构
--
-- 现在部署用: Supabase Postgres
-- 已废弃: Hostinger MariaDB (Web Apps 容器跟 hPanel MySQL 隔离)
--
-- 在 Supabase SQL Editor 跑下面这段 (一行一条, 避免 phpMyAdmin 的解析问题)
-- ==============================================================

-- 第 1 条: 主表 (每次 ESP32 上报一条)
CREATE TABLE IF NOT EXISTS soil_readings (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL,
    ts TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    adc INTEGER NOT NULL,
    pct INTEGER NOT NULL,
    pump_state VARCHAR(8) NOT NULL,
    sensor_err BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX IF NOT EXISTS idx_device_ts ON soil_readings (device_id, ts);
CREATE INDEX IF NOT EXISTS idx_ts ON soil_readings (ts);

-- 第 2 条: 设备表 (可选, 多设备时用)
CREATE TABLE IF NOT EXISTS devices (
    device_id VARCHAR(64) PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    location VARCHAR(64),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_seen TIMESTAMPTZ,
    active BOOLEAN NOT NULL DEFAULT TRUE
);

-- 第 3 条: 远程控制命令队列 (ESP32 上报后 GET /api/cmd 拉取, 执行完 ACK)
CREATE TABLE IF NOT EXISTS commands (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL,
    cmd VARCHAR(32) NOT NULL,
    payload JSONB,
    status VARCHAR(16) NOT NULL DEFAULT 'pending',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    acked_at TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS idx_cmd_device_status ON commands (device_id, status);

-- 第 4 条: 设备配置 (阈值 + 推送周期, 浏览器改完 ESP32 下次上报时拉走)
CREATE TABLE IF NOT EXISTS device_config (
    device_id        VARCHAR(64) PRIMARY KEY,
    on_pct           INTEGER NOT NULL DEFAULT 30  CHECK (on_pct >= 0 AND on_pct <= 99),
    off_pct          INTEGER NOT NULL DEFAULT 70  CHECK (off_pct >= 1 AND off_pct <= 100),
    push_period_ms   INTEGER NOT NULL DEFAULT 300000 CHECK (push_period_ms >= 5000 AND push_period_ms <= 3600000),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- 给默认设备播种一份配置 (后续 ON CONFLICT DO NOTHING 重复执行幂等)
INSERT INTO device_config (device_id, on_pct, off_pct, push_period_ms)
VALUES ('esp32_jh_01', 30, 70, 300000)
ON CONFLICT (device_id) DO NOTHING;

-- ==============================================================
-- 验证用:
-- SELECT tablename FROM pg_tables WHERE schemaname='public';
-- SELECT * FROM soil_readings LIMIT 5;
-- ==============================================================

-- ==============================================================
-- [已废弃] MySQL 旧版 (保留作参考, 不要再跑)
-- ==============================================================
-- CREATE TABLE IF NOT EXISTS soil_readings (
--     id INT AUTO_INCREMENT PRIMARY KEY,
--     device_id VARCHAR(64) NOT NULL,
--     ts DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
--     adc INT NOT NULL,
--     pct INT NOT NULL,
--     pump_state VARCHAR(8) NOT NULL,
--     sensor_err TINYINT(1) NOT NULL DEFAULT 0,
--     INDEX idx_device_ts (device_id, ts),
--     INDEX idx_ts (ts)
-- ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
--
-- CREATE TABLE IF NOT EXISTS devices (
--     device_id VARCHAR(64) PRIMARY KEY,
--     name VARCHAR(64) NOT NULL,
--     location VARCHAR(64) DEFAULT NULL,
--     created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
--     last_seen DATETIME DEFAULT NULL,
--     active TINYINT(1) NOT NULL DEFAULT 1
-- ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;