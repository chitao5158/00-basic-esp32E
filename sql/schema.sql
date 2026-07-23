-- ==============================================================
-- 远程盆栽监控 — 数据库表结构 (Hostinger MariaDB)
-- 简化版: 去掉内联 COMMENT, 拆成两条单独跑
-- ==============================================================

-- 第 1 条: 主表 (每次 ESP32 上报一条)
CREATE TABLE IF NOT EXISTS soil_readings (
    id INT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL,
    ts DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    adc INT NOT NULL,
    pct INT NOT NULL,
    pump_state VARCHAR(8) NOT NULL,
    sensor_err TINYINT(1) NOT NULL DEFAULT 0,
    INDEX idx_device_ts (device_id, ts),
    INDEX idx_ts (ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 第 2 条: 设备表 (可选, 多设备时用)
CREATE TABLE IF NOT EXISTS devices (
    device_id VARCHAR(64) PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    location VARCHAR(64) DEFAULT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen DATETIME DEFAULT NULL,
    active TINYINT(1) NOT NULL DEFAULT 1
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ==============================================================
-- 跑完后, 验证用:
-- SHOW TABLES;
-- DESC soil_readings;
-- ==============================================================