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

        // piggyback: 顺便取一条待执行命令 (在数据推送的响应里塞命令)
        let cmd = null;
        try {
            const cmdResult = await pool.query(
                `SELECT id, cmd, payload FROM commands
                 WHERE device_id = $1 AND status = 'pending'
                 ORDER BY id ASC LIMIT 1`,
                [device_id]
            );
            if (cmdResult.rows[0]) {
                cmd = {
                    id: Number(cmdResult.rows[0].id),
                    name: cmdResult.rows[0].cmd,
                    payload: cmdResult.rows[0].payload ? JSON.parse(cmdResult.rows[0].payload) : null,
                };
            }
        } catch (e) { process.stderr.write(`[ingest] cmd query: ${e.message}\n`); }

        console.log(`[ingest] device=${device_id} adc=${adc} pct=${pct} pump=${pump_state} id=${insert_id} cmd=${cmd ? cmd.name + '#' + cmd.id : 'none'}`);
        return res.json({
            ok: true,
            id: insert_id,
            ts: new Date().toISOString(),
            cmd: cmd,
        });
    } catch (err) {
        process.stderr.write(`[ingest] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- POST /api/command (浏览器 -> 后端入队命令) ---------- */
app.post('/api/command', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const cmd = (req.body && req.body.cmd || '').toString().toUpperCase().trim();
    const device_id = (req.body && req.body.device_id || 'esp32_jh_01').toString().trim();
    const payload = req.body && req.body.payload;

    if (!['WATER', 'STOP', 'REBOOT', 'CALIBRATE'].includes(cmd)) {
        return res.status(400).json({ ok: false, error: 'cmd must be one of: WATER, STOP, REBOOT, CALIBRATE' });
    }

    try {
        const r = await pool.query(
            `INSERT INTO commands (device_id, cmd, payload) VALUES ($1, $2, $3) RETURNING id`,
            [device_id, cmd, payload ? JSON.stringify(payload) : null]
        );
        return res.json({ ok: true, id: r.rows[0].id, cmd, device_id, ts: new Date().toISOString() });
    } catch (err) {
        process.stderr.write(`[command] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- GET /api/cmd (ESP32 主动拉取待执行命令) ---------- */
/* 替代不可靠的 POST 响应 piggyback — ESP-IDF v6 的 esp_http_client
   对 POST 响应 body 读取有 bug, 改用独立 GET 保证命令一定能送达 */
app.get('/api/cmd', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const device_id = (req.query && req.query.device_id || '').toString().trim();
    if (!device_id || device_id.length > 64) {
        return res.status(400).json({ ok: false, error: 'invalid device_id' });
    }

    try {
        const cmdResult = await pool.query(
            `SELECT id, cmd, payload FROM commands
             WHERE device_id = $1 AND status = 'pending'
             ORDER BY id ASC LIMIT 1`,
            [device_id]
        );
        const cmd = cmdResult.rows[0]
            ? {
                id: Number(cmdResult.rows[0].id),
                name: cmdResult.rows[0].cmd,
                payload: cmdResult.rows[0].payload
                    ? (typeof cmdResult.rows[0].payload === 'string'
                        ? JSON.parse(cmdResult.rows[0].payload)
                        : cmdResult.rows[0].payload)
                    : null,
              }
            : null;
        return res.json({ ok: true, cmd });
    } catch (err) {
        process.stderr.write(`[cmd] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- GET /api/config (ESP32 拉取阈值 + 推送周期) ----------
 * 浏览器 PUT 改完后, ESP32 下次 push 时跟 GET /api/cmd 一起拉走.
 * 表里没配置 → 返回默认值 (服务端逻辑, 不强制要求 seed) */
app.get('/api/config', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const device_id = (req.query && req.query.device_id || '').toString().trim();
    if (!device_id || device_id.length > 64) {
        return res.status(400).json({ ok: false, error: 'invalid device_id' });
    }

    const DEFAULT_CONFIG = { on_pct: 30, off_pct: 70, push_period_ms: 300000 };

    try {
        const result = await pool.query(
            `SELECT on_pct, off_pct, push_period_ms FROM device_config WHERE device_id = $1`,
            [device_id]
        );
        const config = result.rows[0]
            ? {
                on_pct:         result.rows[0].on_pct,
                off_pct:        result.rows[0].off_pct,
                push_period_ms: result.rows[0].push_period_ms,
              }
            : DEFAULT_CONFIG;
        return res.json({ ok: true, config });
    } catch (err) {
        process.stderr.write(`[config] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- PUT /api/config (浏览器改阈值 / 推送周期) ---------- */
app.put('/api/config', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const device_id = (req.body && req.body.device_id || 'esp32_jh_01').toString().trim();
    if (!device_id || device_id.length > 64) {
        return res.status(400).json({ ok: false, error: 'invalid device_id' });
    }

    const on_pct         = parseInt(req.body && req.body.on_pct);
    const off_pct        = parseInt(req.body && req.body.off_pct);
    const push_period_ms = parseInt(req.body && req.body.push_period_ms);

    /* 校验 — 服务端权威, ESP32 也会再次验证 */
    if (!Number.isFinite(on_pct) || on_pct < 0 || on_pct > 99) {
        return res.status(400).json({ ok: false, error: 'on_pct must be 0~99' });
    }
    if (!Number.isFinite(off_pct) || off_pct < 1 || off_pct > 100) {
        return res.status(400).json({ ok: false, error: 'off_pct must be 1~100' });
    }
    if (off_pct - on_pct < 5) {
        return res.status(400).json({ ok: false, error: 'off_pct must be at least 5 > on_pct (dead zone)' });
    }
    if (!Number.isFinite(push_period_ms) || push_period_ms < 5000 || push_period_ms > 3600000) {
        return res.status(400).json({ ok: false, error: 'push_period_ms must be 5000~3600000' });
    }

    try {
        await pool.query(
            `INSERT INTO device_config (device_id, on_pct, off_pct, push_period_ms, updated_at)
             VALUES ($1, $2, $3, $4, NOW())
             ON CONFLICT (device_id) DO UPDATE
             SET on_pct = $2, off_pct = $3, push_period_ms = $4, updated_at = NOW()`,
            [device_id, on_pct, off_pct, push_period_ms]
        );
        console.log(`[config] updated device=${device_id} on=${on_pct} off=${off_pct} period=${push_period_ms}ms`);
        return res.json({
            ok: true,
            config: { device_id, on_pct, off_pct, push_period_ms },
            ts: new Date().toISOString(),
        });
    } catch (err) {
        process.stderr.write(`[config] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- POST /api/poll_ack (ESP32 报告命令执行完成) ---------- */
app.post('/api/poll_ack', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const id = parseInt(req.body && req.body.id);
    const status = (req.body && req.body.status || 'done').toString();

    if (!Number.isFinite(id) || id <= 0) {
        return res.status(400).json({ ok: false, error: 'invalid id' });
    }
    if (!['done', 'failed'].includes(status)) {
        return res.status(400).json({ ok: false, error: 'status must be done or failed' });
    }

    try {
        await pool.query(
            `UPDATE commands SET status = $1, acked_at = NOW() WHERE id = $2`,
            [status, id]
        );
        return res.json({ ok: true });
    } catch (err) {
        process.stderr.write(`[poll_ack] DB error: ${err.message}\n`);
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