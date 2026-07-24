/**
 * ESP32 盆栽监控后端 — Node.js + Express + Postgres (Supabase)
 *
 * 路由:
 *   POST /api/ingest    接收 ESP32 上报数据
 *   GET  /api/data      返回最新 + 24h 历史 (JSON)
 *   GET  /api/health    健康检查
 *   GET  /plant/        静态前端 (Chart.js)
 *
 * 部署: Hostinger Cloud Web Apps (Node.js 18+)
 * 数据库: Supabase Postgres (或其他任何 Postgres)
 *
 * 使用 CommonJS (require/module.exports) 而非 ESM, 兼容性更好.
 */

const express = require('express');
const { Pool }  = require('pg');
const dotenv   = require('dotenv');
const path     = require('path');

dotenv.config();

const app = express();

/* ---------- 中间件 ---------- */
app.use(express.json({ limit: '4kb' }));
app.set('trust proxy', 1);

process.stderr.write(`[boot] starting, NODE_ENV=${process.env.NODE_ENV || 'unset'}\n`);

/* ---------- 静态前端 (/plant/) ---------- */
app.use('/plant', express.static(path.join(__dirname, 'public')));
app.get('/plant', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

/* ---------- Postgres 连接池 (Supabase 等) ---------- */
const pool = new Pool({
    connectionString: process.env.DATABASE_URL,
    // Supabase / 大多数托管 Postgres 要求 SSL; rejectUnauthorized: false 容忍自签或 hostname 不匹配
    ssl: process.env.DATABASE_URL
         ? { rejectUnauthorized: false }
         : false,
    max: 5,
    idleTimeoutMillis: 30000,
    connectionTimeoutMillis: 10000,
});

/* ---------- 鉴权辅助 ---------- */
function checkApiKey(req) {
    const provided = (req.body && req.body.key)
                  || (req.query && req.query.key)
                  || req.headers['x-api-key']
                  || '';
    return typeof provided === 'string'
        && provided.length > 0
        && provided === process.env.API_KEY;
}

/* ---------- POST /api/ingest ---------- */
app.post('/api/ingest', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }

    const device_id  = (req.body && req.body.device_id || '').toString().trim();
    const adc        = parseInt(req.body && req.body.adc);
    const pct        = parseInt(req.body && req.body.pct);
    const pump_raw   = (req.body && req.body.pump || '').toString().toUpperCase().trim();
    const sensor_err = (req.body && req.body.sensor_err) ? true : false;

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
        // Postgres 用 $1, $2... 占位符; INSERT...RETURNING id 拿刚插入的 id
        const insertResult = await pool.query(
            `INSERT INTO soil_readings (device_id, adc, pct, pump_state, sensor_err)
             VALUES ($1, $2, $3, $4, $5)
             RETURNING id`,
            [device_id, adc, pct, pump_state, sensor_err]
        );
        const insert_id = insertResult.rows[0].id;

        // 顺带更新 devices.last_seen (表可能不存在, 忽略)
        await pool.query(
            `UPDATE devices SET last_seen = NOW() WHERE device_id = $1`,
            [device_id]
        ).catch(() => {});

        console.log(`[ingest] device=${device_id} adc=${adc} pct=${pct} pump=${pump_state} id=${insert_id}`);
        return res.json({ ok: true, id: insert_id, ts: new Date().toISOString() });
    } catch (err) {
        process.stderr.write(`[ingest] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- GET /api/data ---------- */
app.get('/api/data', async (req, res) => {
    try {
        const latestResult = await pool.query(
            `SELECT device_id, ts, adc, pct, pump_state, sensor_err
             FROM soil_readings ORDER BY id DESC LIMIT 1`
        );
        const latest = latestResult.rows[0] || null;
        // Postgres TIMESTAMPTZ 返回 JS Date 对象, 转 ISO 字符串
        if (latest) latest.ts = latest.ts.toISOString();

        const historyResult = await pool.query(
            `SELECT ts, pct, pump_state
             FROM soil_readings
             WHERE ts >= NOW() - INTERVAL '24 hours'
             ORDER BY ts ASC`
        );

        const totalResult = await pool.query(
            `SELECT COUNT(*) AS c FROM soil_readings`
        );

        return res.json({
            latest,
            history: historyResult.rows.map(r => ({
                ts:   r.ts.toISOString(),
                pct:  r.pct,
                pump: r.pump_state,
            })),
            total_readings: parseInt(totalResult.rows[0].c, 10),
        });
    } catch (err) {
        process.stderr.write(`[data] DB error: ${err.message}\n`);
        return res.status(500).json({ error: 'db error' });
    }
});

/* ---------- GET /api/health ---------- */
app.get('/api/health', (req, res) => {
    res.json({
        ok: true,
        ts: new Date().toISOString(),
        uptime: process.uptime(),
    });
});

/* ---------- 404 fallback ---------- */
app.use((req, res) => {
    res.status(404).json({ error: 'not found', path: req.path });
});

const PORT = parseInt(process.env.PORT || '3000', 10);
app.listen(PORT, '0.0.0.0', () => {
    process.stderr.write(`[boot] listening on 0.0.0.0:${PORT} (cwd=${process.cwd()})\n`);
    console.log(`[plant-monitor] listening on port ${PORT}`);
});