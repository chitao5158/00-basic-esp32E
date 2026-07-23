<?php
/**
 * 配置文件模板 — 真实配置请复制为 config.php 并填入真实值
 *
 * 部署步骤:
 *   1. cp api/config.example.php api/config.php
 *   2. 编辑 api/config.php, 填入 Hostinger DB 凭据 + 自定义 API Key
 *   3. 上传整个 api/ 目录到 Hostinger public_html/api/
 *   4. ⚠️ 确认 .htaccess 已上传, 防止直接访问 config.php
 *
 * 真实 config.php 在 .gitignore 中, 不会被提交.
 */

// === Database (Hostinger hPanel → 数据库 → MySQL) ===
define('DB_HOST', 'localhost');                 // 共享主机通常就是 localhost
define('DB_NAME', 'YOUR_DB_NAME');             // 例如 u138723766_afl_jh
define('DB_USER', 'YOUR_DB_USER');             // 通常与 DB_NAME 相同
define('DB_PASS', 'YOUR_DB_PASSWORD');         // hPanel 创建库时设置的密码

// === API Key (ESP32 固件 DEVICE_API_KEY 必须与此一致) ===
// 生成方法: openssl rand -hex 32 或 password_hash(随机串, PASSWORD_DEFAULT)
define('API_KEY', 'CHANGE_ME_TO_A_LONG_RANDOM_STRING_AT_LEAST_32_CHARS');

// === 时区 ===
date_default_timezone_set('Asia/Shanghai');     // 根据实际时区调整

// === 调试 ===
// 上线后改成 false 隐藏 PHP 错误细节
define('DEBUG', false);