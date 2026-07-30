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

        /* 顺便处理 bundle 在 body 里的 pump_events 数组 (ESP32 本地 buffer
         * 攒着的水泵启停事件). 每个 event 要么 INSERT start, 要么 UPDATE 配对 stop. */
        const pump_events = Array.isArray(req.body && req.body.pump_events) ? req.body.pump_events : [];
        let pump_events_inserted = 0;
        for (const evt of pump_events) {
            try {
                if (evt.event === 'start') {
                    const trigger = (evt.trigger || 'auto').toString();
                    const start_pct = parseInt(evt.start_pct);
                    const start_ts_ms = parseInt(evt.start_ts_ms);
                    if (Number.isFinite(start_ts_ms) && start_ts_ms > 0
                        && ['auto','web','manual'].includes(trigger)
                        && Number.isFinite(start_pct) && start_pct >= 0 && start_pct <= 100) {
                        await pool.query(
                            `INSERT INTO pump_events (device_id, start_ts, trigger, start_pct)
                             VALUES ($1, to_timestamp($2 / 1000.0), $3, $4)`,
                            [device_id, start_ts_ms, trigger, start_pct]
                        );
                        pump_events_inserted++;
                    }
                } else if (evt.event === 'stop') {
                    const end_pct = parseInt(evt.end_pct);
                    const start_ts_ms = parseInt(evt.start_ts_ms);
                    const duration_ms = parseInt(evt.duration_ms);
                    if (Number.isFinite(start_ts_ms) && start_ts_ms > 0
                        && Number.isFinite(end_pct) && end_pct >= 0 && end_pct <= 100
                        && Number.isFinite(duration_ms) && duration_ms >= 0 && duration_ms <= 600000) {
                        const duration_sec = Math.round(duration_ms / 1000);
                        const r = await pool.query(
                            `UPDATE pump_events
                             SET end_ts = NOW(), duration_sec = $1, end_pct = $2
                             WHERE device_id = $3
                               AND start_ts = to_timestamp($4 / 1000.0)
                               AND end_ts IS NULL`,
                            [duration_sec, end_pct, device_id, start_ts_ms]
                        );
                        if (r.rowCount === 0) {
                            /* 没找到 start 行 (ESP32 重启丢了 start), 兜底新建一行 */
                            await pool.query(
                                `INSERT INTO pump_events (device_id, start_ts, end_ts, duration_sec, trigger, end_pct)
                                 VALUES ($1, to_timestamp($2 / 1000.0), NOW(), $3, 'web', $4)`,
                                [device_id, start_ts_ms, duration_sec, end_pct]
                            );
                        }
                        pump_events_inserted++;
                    }
                }
            } catch (e) { process.stderr.write(`[ingest] pump_event insert: ${e.message}\n`); }
        }
        if (pump_events_inserted > 0) {
            console.log(`[ingest] pump_events inserted: ${pump_events_inserted}`);
        }

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

/* ---------- POST /api/pump_event (ESP32 上报水泵启停) ---------- */
/* body: { event: 'start'|'stop', device_id, trigger?, start_pct?, end_pct?, start_ts_ms?, duration_ms? }
 * start_ts_ms 是 ESP32 给的 epoch ms (来自本地 tick), 用它做关联 — 这样 stop 不用记 id */
app.post('/api/pump_event', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const event = (req.body && req.body.event || '').toString();
    const device_id = (req.body && req.body.device_id || '').toString().trim();
    if (!['start', 'stop'].includes(event)) {
        return res.status(400).json({ ok: false, error: 'event must be start or stop' });
    }
    if (!device_id || device_id.length > 64) {
        return res.status(400).json({ ok: false, error: 'invalid device_id' });
    }

    const start_ts_ms = parseInt(req.body && req.body.start_ts_ms);
    if (!Number.isFinite(start_ts_ms) || start_ts_ms <= 0) {
        return res.status(400).json({ ok: false, error: 'invalid start_ts_ms' });
    }

    try {
        if (event === 'start') {
            const trigger   = (req.body && req.body.trigger || 'auto').toString();
            const start_pct = parseInt(req.body && req.body.start_pct);
            if (!['auto', 'web', 'manual'].includes(trigger)) {
                return res.status(400).json({ ok: false, error: 'trigger must be auto|web|manual' });
            }
            if (!Number.isFinite(start_pct) || start_pct < 0 || start_pct > 100) {
                return res.status(400).json({ ok: false, error: 'invalid start_pct (0~100)' });
            }
            await pool.query(
                `INSERT INTO pump_events (device_id, start_ts, trigger, start_pct)
                 VALUES ($1, to_timestamp($2 / 1000.0), $3, $4)`,
                [device_id, start_ts_ms, trigger, start_pct]
            );
            console.log(`[pump_event] start device=${device_id} trigger=${trigger} pct=${start_pct}`);
            return res.json({ ok: true, event: 'start', start_ts_ms });
        } else {
            /* stop — 用 start_ts_ms 找原行 (前提: ESP32 传来的 start_ts_ms 与 start 时一致) */
            const end_pct     = parseInt(req.body && req.body.end_pct);
            const duration_ms = parseInt(req.body && req.body.duration_ms);
            if (!Number.isFinite(end_pct) || end_pct < 0 || end_pct > 100) {
                return res.status(400).json({ ok: false, error: 'invalid end_pct (0~100)' });
            }
            if (!Number.isFinite(duration_ms) || duration_ms < 0 || duration_ms > 600000) {
                return res.status(400).json({ ok: false, error: 'invalid duration_ms (0~600000)' });
            }
            const duration_sec = Math.round(duration_ms / 1000);
            const r = await pool.query(
                `UPDATE pump_events
                 SET end_ts = NOW(), duration_sec = $1, end_pct = $2
                 WHERE device_id = $3
                   AND start_ts = to_timestamp($4 / 1000.0)
                   AND end_ts IS NULL`,
                [duration_sec, end_pct, device_id, start_ts_ms]
            );
            if (r.rowCount === 0) {
                /* 没找到 start 行 — 可能是 ESP32 重启后丢上下文. 兜底: 新建一行只填 end. */
                await pool.query(
                    `INSERT INTO pump_events (device_id, start_ts, end_ts, duration_sec, trigger, end_pct)
                     VALUES ($1, to_timestamp($2 / 1000.0), NOW(), $3, 'web', $4)`,
                    [device_id, start_ts_ms, duration_sec, end_pct]
                );
            }
            console.log(`[pump_event] stop device=${device_id} duration=${duration_ms}ms pct=${end_pct}`);
            return res.json({ ok: true, event: 'stop', duration_sec });
        }
    } catch (err) {
        process.stderr.write(`[pump_event] DB error: ${err.message}\n`);
        return res.status(500).json({ ok: false, error: 'db error' });
    }
});

/* ---------- GET /api/pump_events (前端浇水历史) ---------- */
app.get('/api/pump_events', async (req, res) => {
    if (!checkApiKey(req)) {
        return res.status(401).json({ ok: false, error: 'invalid API key' });
    }
    const device_id = (req.query && req.query.device_id || '').toString().trim();
    if (!device_id || device_id.length > 64) {
        return res.status(400).json({ ok: false, error: 'invalid device_id' });
    }
    const limit = Math.min(parseInt(req.query.limit) || 50, 200);

    try {
        const result = await pool.query(
            `SELECT id, start_ts, end_ts, duration_sec, trigger, start_pct, end_pct
             FROM pump_events
             WHERE device_id = $1
             ORDER BY start_ts DESC
             LIMIT $2`,
            [device_id, limit]
        );
        return res.json({
            ok: true,
            events: result.rows.map(r => ({
                id:           Number(r.id),
                start_ts:     r.start_ts.toISOString(),
                end_ts:       r.end_ts ? r.end_ts.toISOString() : null,
                duration_sec: r.duration_sec,
                trigger:      r.trigger,
                start_pct:    r.start_pct,
                end_pct:      r.end_pct,
                running:      r.end_ts === null,  /* 还在跑 */
            })),
        });
    } catch (err) {
        process.stderr.write(`[pump_events] DB error: ${err.message}\n`);
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