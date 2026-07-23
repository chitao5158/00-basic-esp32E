<?php
/**
 * ESP32 土壤湿度上报端点
 *
 * URL: POST https://afl.cn/api/ingest.php
 *
 * 请求体 (JSON):
 *   {
 *     "key":         "API_KEY",        // 必须匹配 config.php 的 API_KEY
 *     "device_id":   "esp32_jh_01",    // 设备唯一 ID
 *     "adc":         2517,             // 原始 ADC (0~4095)
 *     "pct":         45,               // 湿度百分比 (0~100)
 *     "pump":        "OFF",            // ON / OFF / LOCKED
 *     "sensor_err":  false             // true / false
 *   }
 *
 * 响应 (JSON):
 *   { "ok": true, "id": 123, "ts": "2026-07-23T12:34:56+08:00" }
 *   { "ok": false, "error": "原因" }
 */

require_once __DIR__ . '/config.php';

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

if (DEBUG) {
    ini_set('display_errors', '1');
    error_reporting(E_ALL);
} else {
    ini_set('display_errors', '0');
}

/* ---------- 1. 仅接受 POST ---------- */
if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['ok' => false, 'error' => 'method not allowed, use POST']);
    exit;
}

/* ---------- 2. 解析 JSON ---------- */
$raw = file_get_contents('php://input');
$data = json_decode($raw, true);
if (!is_array($data)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid JSON body']);
    exit;
}

/* ---------- 3. API Key 校验 ---------- */
// 支持三种传递方式: ?key=, body.key, 或 X-API-Key header
$provided = $data['key'] ?? ($_GET['key'] ?? ($_SERVER['HTTP_X_API_KEY'] ?? ''));
if (!is_string($provided) || !hash_equals(API_KEY, $provided)) {
    http_response_code(401);
    echo json_encode(['ok' => false, 'error' => 'invalid API key']);
    exit;
}

/* ---------- 4. 字段校验 ---------- */
$device_id  = isset($data['device_id']) ? trim((string)$data['device_id']) : '';
$adc        = isset($data['adc']) ? (int)$data['adc'] : -1;
$pct        = isset($data['pct']) ? (int)$data['pct'] : -1;
$pump_state = isset($data['pump']) ? strtoupper(trim((string)$data['pump'])) : '';
$sensor_err = !empty($data['sensor_err']) ? 1 : 0;

if ($device_id === '' || strlen($device_id) > 64) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid device_id']);
    exit;
}
if ($adc < 0 || $adc > 4095) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid adc (0~4095)']);
    exit;
}
if ($pct < 0 || $pct > 100) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid pct (0~100)']);
    exit;
}
if (!in_array($pump_state, ['ON', 'OFF', 'LOCKED'], true)) {
    $pump_state = 'OFF';
}

/* ---------- 5. 写入数据库 ---------- */
$mysqli = @new mysqli(DB_HOST, DB_USER, DB_PASS, DB_NAME);
if ($mysqli->connect_errno) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'db connect failed: ' . $mysqli->connect_error]);
    exit;
}
$mysqli->set_charset('utf8mb4');

$stmt = $mysqli->prepare(
    'INSERT INTO soil_readings (device_id, adc, pct, pump_state, sensor_err)
     VALUES (?, ?, ?, ?, ?)'
);
if (!$stmt) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'prepare failed: ' . $mysqli->error]);
    exit;
}
$stmt->bind_param('siisi', $device_id, $adc, $pct, $pump_state, $sensor_err);

if (!$stmt->execute()) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'insert failed: ' . $stmt->error]);
    exit;
}

/* ---------- 6. 更新设备 last_seen (如果有记录) ---------- */
$mysqli->query(
    "UPDATE devices SET last_seen = NOW() WHERE device_id = '" .
    $mysqli->real_escape_string($device_id) . "'"
);

$insert_id = $stmt->insert_id;
$stmt->close();
$mysqli->close();

http_response_code(200);
echo json_encode([
    'ok' => true,
    'id' => $insert_id,
    'ts' => date('c'),
]);