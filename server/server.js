/**
 * ESP32 盆栽监控后端 — Node.js + Express + MariaDB
 *
 * 路由:
 *   POST /api/ingest    接收 ESP32 上报数据
 *   GET  /api/data      返回最新 + 24h 历史 (JSON)
 *   GET  /api/health    健康检查
 *   GET  /plant/        静态前端 (Chart.js)
 *
 * 部署: Hostinger Cloud Web Apps (Node.js 18+) 或任何支持 Node 的平台
 */

import express from 'express';
import mysql from 'mysql2/promise';
import dotenv from 'dotenv';
import path from 'path';
import { fileURLToPath } from 'url';

dotenv.config();

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();

/* ---------- 中间件 ---------- */
app.use(express.json({ limit: '4kb' }));

// 让 express 信任 1 层代理 (Hostinger nginx 在前面)
app.set('trust proxy', 1);

/* ---------- 静态前端 (/plant/) ---------- */
app.use('/plant', express.static(path.join(__dirname, 'public')));

// 兼容直接访问 /plant/ 时返回 index.html
app.get('/plant', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

/* ---------- MySQL 连接池 ---------- */
const pool = mysql.createPool({
    host:               process.env.DB_HOST || 'localhost',
    port:               parseInt(process.env.DB_PORT || '3306', 10),
    user:               process.env.DB_USER,
    password:           process.env.DB_PASS,
    database:           process.env.DB_NAME,
    waitForConnections: true,
    connectionLimit:    5,
    queueLimit:         0,
    charset:            'utf8mb4',
});

/* ---------- 鉴权辅助 ---------- */
function checkApiKey(req) {
    const provided = req.body?.key
                   ?? req.query?.key
                   ?? req.headers['x-api-key']
                   ?? '';
    return typeof provided === 'string'
        && provided.length > 0
        && provided === process.env.API_KEY;
}

/* ---------- POST /api/ingest ---------- */
app.post('/api/ingest', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }

    const device_id  = (req.body?.device_id || '').toString().trim();
    const adc        = parseInt(req.body?.adc);
    const pct        = parseInt(req.body?.pct);
    const pump_raw   = (req.body?.pump || '').toString().toUpperCase().trim();
    const sensor_err = req.body?.sensor_err ? 1 : 0;

    if (!device_id || device_id.length > 64) {
        return res.status(400).json({ ok: false, error: 'invalid device_id' });
    }
    if (!Number.isFinite(adc) || adc < 0 || adc > 4095) {
        return res.status(400).json({ ok: false, error: 'invalid adc (0~4095)' });
    }
    if (!Number.isFinite(pct) || pct < 0 || pct > 100) {
        return res.status(400).json({ ok: false, error: 'invalid pct (0~100)' });
    }
    const pump_state = ['ON', 'OFF', 'LOCKED'].includes(pump_raw) ? pump_raw : 'OFF';

    try {
        const [result] = await pool.execute(
            `INSERT INTO soil_readings (device_id, adc, pct, pump_state, sensor_err)
             VALUES (?, ?, ?, ?, ?)`,
            [device_id, adc, pct, pump_state, sensor_err]
        );

        // 更新 devices.last_seen (如果设备已注册)
        await pool.execute(
            `UPDATE devices SET last_seen = NOW() WHERE device_id = ?`,
            [device_id]
        ).catch(() => {/* 表可能不存在, 忽略 */});

        console.log(`[ingest] device=${device_id} adc=${adc} pct=${pct} pump=${pump_state} id=${result.insertId}`);
        return res.json({ ok: true, id: result.insertId, ts: new Date().toISOString() });
    } catch (err) {
        console.error('[ingest] DB error:', err);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- GET /api/data ---------- */
app.get('/api/data', async (req, res) => {
    try {
        const [latestRows] = await pool.execute(
            `SELECT device_id, ts, adc, pct, pump_state, sensor_err
             FROM soil_readings ORDER BY id DESC LIMIT 1`
        );
        const latest = latestRows[0] || null;
        if (latest) {
            // mysql2 datetime 转 ISO 字符串
            latest.ts = new Date(latest.ts).toISOString();
        }

        const [historyRows] = await pool.execute(
            `SELECT ts, pct, pump_state
             FROM soil_readings
             WHERE ts >= DATE_SUB(NOW(), INTERVAL 24 HOUR)
             ORDER BY ts ASC`
        );

        const [totalRows] = await pool.execute(
            `SELECT COUNT(*) AS c FROM soil_readings`
        );

        return res.json({
            latest,
            history: historyRows.map(r => ({
                ts: new Date(r.ts).toISOString(),
                pct: r.pct,
                pump: r.pump_state,
            })),
            total_readings: totalRows[0].c,
        });
    } catch (err) {
        console.error('[data] DB error:', err);
        return res.status(500).json({ error: 'db error' });
    }
});

/* ---------- GET /api/health ---------- */
app.get('/api/health', (req, res) => {
    res.json({ ok: true, ts: new Date().toISOString(), uptime: process.uptime() });
});

/* ---------- 404 fallback ---------- */
app.use((req, res) => {
    res.status(404).json({ error: 'not found', path: req.path });
});

const PORT = parseInt(process.env.PORT || '3000', 10);
app.listen(PORT, '0.0.0.0', () => {
    console.log(`[plant-monitor] listening on 0.0.0.0:${PORT}`);
});