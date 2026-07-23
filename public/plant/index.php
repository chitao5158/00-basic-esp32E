<?php
/**
 * 盆栽实时监控 — afl.cn/plant/
 *
 * 部署路径: public_html/plant/index.php (afl.cn 根目录下的 plant 子目录)
 *
 * 功能:
 *   - 显示最新一条土壤湿度读数 + 水泵状态
 *   - 24 小时湿度折线图 (Chart.js via CDN)
 *   - 自动每 5 分钟刷新页面
 */

require __DIR__ . '/../../api/config.php';

date_default_timezone_set('Asia/Shanghai');

/* ---------- 数据库连接 ---------- */
$mysqli = @new mysqli(DB_HOST, DB_USER, DB_PASS, DB_NAME);
$db_error = null;
if ($mysqli->connect_errno) {
    $db_error = '数据库连接失败: ' . htmlspecialchars($mysqli->connect_error);
}

/* ---------- 查询数据 ---------- */
$latest = null;
$history = [];
$device_count = 0;
$total_readings = 0;

if (!$db_error) {
    $mysqli->set_charset('utf8mb4');

    /* 最新一条 */
    $stmt = $mysqli->prepare(
        'SELECT device_id, ts, adc, pct, pump_state, sensor_err
         FROM soil_readings ORDER BY id DESC LIMIT 1'
    );
    if ($stmt && $stmt->execute()) {
        $latest = $stmt->get_result()->fetch_assoc();
    }
    $stmt && $stmt->close();

    /* 24h 历史 */
    $since = date('Y-m-d H:i:s', strtotime('-24 hours'));
    $stmt = $mysqli->prepare(
        "SELECT ts, pct, pump_state FROM soil_readings
         WHERE ts >= ? ORDER BY ts ASC"
    );
    if ($stmt && $stmt->execute()) {
        $result = $stmt->get_result();
        while ($row = $result->fetch_assoc()) {
            $history[] = $row;
        }
    }
    $stmt && $stmt->close();

    /* 设备总数 + 总记录数 */
    $r = $mysqli->query('SELECT COUNT(*) FROM devices WHERE active = 1');
    if ($r) $device_count = (int)$r->fetch_row()[0];

    $r = $mysqli->query('SELECT COUNT(*) FROM soil_readings');
    if ($r) $total_readings = (int)$r->fetch_row()[0];

    $mysqli->close();
}

/* ---------- 准备图表数据 ---------- */
$labels = [];
$pcts   = [];
$pumps  = [];
foreach ($history as $h) {
    $labels[] = date('H:i', strtotime($h['ts']));
    $pcts[]   = (int)$h['pct'];
    $pumps[]  = $h['pump_state'] === 'ON' ? 1 : 0;
}
?>
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>afl.cn · 盆栽监控</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC", sans-serif;
            background: linear-gradient(135deg, #e8f5e9 0%, #c8e6c9 100%);
            min-height: 100vh;
            color: #1b3a1b;
            padding: 20px;
        }
        .container { max-width: 900px; margin: 0 auto; }
        header {
            text-align: center;
            padding: 30px 0;
        }
        header h1 { font-size: 32px; margin-bottom: 8px; }
        header .subtitle { font-size: 14px; opacity: 0.7; }

        .status-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
            gap: 16px;
            margin-bottom: 24px;
        }
        .card {
            background: white;
            border-radius: 12px;
            padding: 20px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.08);
        }
        .card .label { font-size: 13px; opacity: 0.6; margin-bottom: 8px; }
        .card .value { font-size: 32px; font-weight: 600; }
        .card .value.pump-on   { color: #1976d2; }
        .card .value.pump-off  { color: #757575; }
        .card .value.pump-lock { color: #d32f2f; }
        .card.sensor-err { background: #ffebee; border: 2px solid #d32f2f; }

        .chart-card {
            background: white;
            border-radius: 12px;
            padding: 24px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.08);
            margin-bottom: 24px;
        }
        .chart-card h2 { font-size: 18px; margin-bottom: 16px; }
        .chart-container { position: relative; height: 320px; }

        .empty-state {
            text-align: center;
            padding: 60px 20px;
            opacity: 0.6;
        }

        .bar-container {
            margin-top: 12px;
            background: #f0f0f0;
            border-radius: 8px;
            height: 12px;
            overflow: hidden;
        }
        .bar-fill {
            height: 100%;
            background: linear-gradient(90deg, #66bb6a, #43a047);
            transition: width 0.5s;
        }

        .footer {
            text-align: center;
            font-size: 12px;
            opacity: 0.5;
            margin-top: 30px;
            padding-bottom: 20px;
        }

        @media (max-width: 600px) {
            body { padding: 12px; }
            header h1 { font-size: 24px; }
            .card .value { font-size: 24px; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>🌱 盆栽实时监控</h1>
            <div class="subtitle">afl.cn · 每 5 分钟自动刷新</div>
        </header>

        <?php if ($db_error): ?>
            <div class="card sensor-err">
                <div class="label">⚠️ 系统错误</div>
                <div class="value" style="font-size:16px;"><?= $db_error ?></div>
            </div>
        <?php elseif (!$latest): ?>
            <div class="card">
                <div class="empty-state">
                    <p>📡 暂无数据</p>
                    <p style="margin-top:8px; font-size:13px;">
                        ESP32 还没上报过数据，或检查 wifi/网络是否正常
                    </p>
                </div>
            </div>
        <?php else:
            $is_err = (int)$latest['sensor_err'] === 1;
            $pump_class = 'pump-off';
            if ($is_err) $pump_class = 'pump-lock';
            else if ($latest['pump_state'] === 'ON') $pump_class = 'pump-on';
            $pump_cn = ['ON' => '运行中', 'OFF' => '已停止', 'LOCKED' => '已锁定'][ $latest['pump_state'] ] ?? $latest['pump_state'];
        ?>
            <div class="status-grid">
                <div class="card<?= $is_err ? ' sensor-err' : '' ?>">
                    <div class="label">土壤湿度</div>
                    <div class="value"><?= (int)$latest['pct'] ?>%</div>
                    <div class="bar-container">
                        <div class="bar-fill" style="width:<?= (int)$latest['pct'] ?>%"></div>
                    </div>
                </div>
                <div class="card">
                    <div class="label">水泵状态</div>
                    <div class="value <?= $pump_class ?>"><?= htmlspecialchars($pump_cn) ?></div>
                </div>
                <div class="card">
                    <div class="label">设备</div>
                    <div class="value" style="font-size:18px;"><?= htmlspecialchars($latest['device_id']) ?></div>
                </div>
                <div class="card">
                    <div class="label">原始 ADC</div>
                    <div class="value"><?= (int)$latest['adc'] ?></div>
                </div>
            </div>

            <div class="chart-card">
                <h2>📈 最近 24 小时土壤湿度</h2>
                <?php if (empty($history)): ?>
                    <div class="empty-state">暂无历史数据</div>
                <?php else: ?>
                    <div class="chart-container">
                        <canvas id="chart"></canvas>
                    </div>
                <?php endif; ?>
            </div>

            <div class="chart-card" style="font-size:13px; opacity:0.8;">
                <strong>📊 总览</strong> &nbsp;
                设备数: <strong><?= $device_count ?></strong> &nbsp;·&nbsp;
                总采样: <strong><?= $total_readings ?></strong> 条 &nbsp;·&nbsp;
                最近上报: <strong><?= htmlspecialchars($latest['ts']) ?></strong>
            </div>
        <?php endif; ?>

        <div class="footer">
            自动刷新: <meta http-equiv="refresh" content="300"> 每 5 分钟
        </div>
    </div>

    <?php if (!empty($history)): ?>
    <script>
        const ctx = document.getElementById('chart').getContext('2d');
        new Chart(ctx, {
            type: 'line',
            data: {
                labels: <?= json_encode($labels, JSON_UNESCAPED_UNICODE) ?>,
                datasets: [{
                    label: '湿度 %',
                    data: <?= json_encode($pcts) ?>,
                    borderColor: 'rgb(67, 160, 71)',
                    backgroundColor: 'rgba(67, 160, 71, 0.15)',
                    fill: true,
                    tension: 0.3,
                    pointRadius: 2,
                    pointHoverRadius: 5,
                }, {
                    label: '水泵 (1=ON)',
                    data: <?= json_encode($pumps) ?>,
                    borderColor: 'rgb(25, 118, 210)',
                    backgroundColor: 'transparent',
                    borderDash: [4, 4],
                    tension: 0,
                    pointRadius: 0,
                    yAxisID: 'y1',
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                interaction: { mode: 'index', intersect: false },
                plugins: {
                    legend: { position: 'bottom' },
                    tooltip: {
                        callbacks: {
                            afterBody: (items) => {
                                const idx = items[0].dataIndex;
                                const pump = <?= json_encode($pumps) ?>[idx];
                                return pump === 1 ? '💧 水泵运行中' : '';
                            }
                        }
                    }
                },
                scales: {
                    x: { ticks: { maxTicksLimit: 12 } },
                    y: {
                        min: 0, max: 100,
                        title: { display: true, text: '湿度 %' },
                    },
                    y1: {
                        min: 0, max: 1, display: false,
                    }
                }
            }
        });
    </script>
    <?php endif; ?>
</body>
</html>