# ESP32 自动浇花系统

基于 **绿深 ESP32-E**（ESP32-WROOM-32E 模块，TYPE-C 接口，板载 CH340C）开发板的家庭自动浇花系统：

- SSD1306 OLED 实时显示土壤湿度
- 土壤湿度传感器（电阻式）周期性采集
- 继电器驱动水泵，**hysteresis 控制**自动浇花
- 多重安全机制（传感器合理性 + 水泵最长运行时间）

## 硬件

| 器件 | 型号 | 接口 |
|---|---|---|
| 主控 | 绿深 ESP32-E (WROOM-32E, D0WD-V3 rev3, TYPE-C, CH340C) | — |
| 显示屏 | SSD1306 128×32 OLED | I2C @ 0x3C |
| 土壤湿度传感器 | 电阻式（探针裸露型） | ADC + GPIO 控制 VCC |
| 继电器模块 | SRD-05VDC-SL-C（5V 低电平触发） | GPIO 控制 |
| 水泵（用户自备） | 5V/12V 直流小水泵 或 220V 交流水泵 | 接继电器 COM-NO |

## 接线总览

### OLED（SSD1306）

| 模块引脚 | 扩展板丝印 | ESP32 GPIO | 说明 |
|---|---|---|---|
| OLED GND | GND | — | |
| OLED VDD | 3V3 | — | 该模块 3V3 / 5V 均可点亮，默认接 3V3 |
| OLED SCK | P22 | GPIO22 | I2C SCL |
| OLED SDA | P21 | GPIO21 | I2C SDA |

### 土壤湿度传感器（**VCC 必须接 GPIO18 不要接 3V3**）

| 模块引脚 | 扩展板丝印 | ESP32 GPIO | 说明 |
|---|---|---|---|
| VCC | **P18**（不是 3V3！） | GPIO18 | 用 GPIO 控制通电，防探针电化学腐蚀 |
| GND | GND | — | |
| AO | P33 | GPIO33 | ADC1_CH5 |
| DO | (悬空) | — | 本项目未使用 |

> **为什么 VCC 要用 GPIO 控制？** 电阻式传感器长时间通 DC 电会电解腐蚀探针，adc 读数会缓慢漂移到「看起来很干」的状态。把 VCC 接到 GPIO 后，每次读数前才上电 10ms、读完后立即断电，占空比 ~3.7%，腐蚀速度降低 20+ 倍。

### 继电器模块

| 模块引脚 | 扩展板丝印 | ESP32 GPIO | 说明 |
|---|---|---|---|
| VCC | **5V**（不是 3V3！） | — | 继电器线圈需 5V 驱动，3V3 拉不动 |
| GND | GND | — | |
| IN | P5 | GPIO5 | **低电平触发**（多数 5V 模块默认） |

### 高压侧（水泵电源回路）

```
[水泵电源 (+)] ──► 继电器 COM ──► 继电器 NO ──► 水泵 (+)
[水泵电源 (-)] ───────────────────► 水泵 (-)
```

- **5V/12V 直流小水泵**：电源与扩展板 5V/GND 共用或独立 12V 适配器
- **220V 交流水泵**：⚠️ 必须断电接线，加保险丝，使用绝缘外壳
- **DC 电机必须加续流二极管**（1N4007，阴极朝 +）跨接在水泵两端

### 引脚分配一览

| 用途 | 扩展板丝印 | ESP32 GPIO | 备注 |
|---|---|---|---|
| I2C SCL | P22 | GPIO22 | OLED |
| I2C SDA | P21 | GPIO21 | OLED |
| 土壤湿度 AO | P33 | GPIO33 | ADC1_CH5 |
| 土壤湿度 VCC | P18 | GPIO18 | GPIO 控制通电 |
| 继电器 IN | P5 | GPIO5 | 低电平触发 |

## 关键配置

板级参数已固化在 `sdkconfig.defaults`（重新生成 `sdkconfig` 时自动应用）：

| 项 | 值 |
|---|---|
| Target | esp32 |
| 芯片版本 | rev 3（D0WD-V3） |
| Flash | 4MB / 80MHz / DIO |
| 分区表 | 自定义 `partitions-4Mib.csv` |
| 控制台 | UART0 @115200 |

`main/CMakeLists.txt` 依赖：

```cmake
REQUIRES ssd1306 esp_adc esp_driver_gpio
```

> ESP-IDF v6 把 `driver/*.h` 拆成独立组件（`esp_driver_gpio` / `esp_driver_i2c` 等），必须显式列在 REQUIRES。

## 编译与烧录

需已安装 **ESP-IDF 6.0.1** 并激活环境（`. $IDF_PATH/export.sh`）。

```bash
# 编译
idf.py build

# 烧录 + 串口监视（替换为你的串口）
idf.py -p /dev/cu.wchusbserial-XXXX flash monitor
# 退出监视: Ctrl + ]
```

> CH340C 在 macOS 上一般显示为 `/dev/cu.wchusbserial-*`；插上后用 `ls /dev/cu.*` 查看实际设备名。

VS Code ESP-IDF 扩展：确保 `.vscode/settings.json` 中 `"idf.flashType": "UART"`（本板无板载 JTAG），然后点 **ESP-IDF: Flash**。

首次或换 IDF 版本导致构建报 Python 环境不匹配时：

```bash
idf.py fullclean && idf.py build
```

## 控制逻辑

### 自动浇花阈值（hysteresis）

```c
if (pct < 30)        want_on = true;     // 太干 → 开泵
else if (pct >= 70)  want_on = false;    // 够湿 → 关泵
else                 want_on = s_pump_on; // 死区 → 保持现状
```

死区 30%~70% 防止在临界值附近水泵反复跳。阈值在 [main/main.c](main/main.c) 中可调：

```c
#define PUMP_ON_PCT         30      /* 低于此值启泵 */
#define PUMP_OFF_PCT        70      /* 高于此值关泵 */
```

### 安全机制（双重保险）

```c
#define PUMP_MAX_RUNTIME_MS 60000   /* 水泵最长连续运行 60s 强制关闭 */
#define SENSOR_MIN_VALID    200     /* adc < 此值视为短路/接线错误 */
#define SENSOR_MAX_VALID    4095    /* adc > 此值视为断路 (= 满量程) */
```

| 异常场景 | 触发机制 | 行为 |
|---|---|---|
| 传感器拔出 / 接线松脱 | `adc > 4095` 或 adc < 200 | 立刻强制关泵，OLED 显示 SENSOR ERR |
| 土壤根本不吸水 / 水泵空转 / 水源耗尽 | 水泵连续 ON 超过 60s | 强制关泵并打印 `水泵已连续运行 60000ms` |
| ADC 卡在干值（如 4095） | 由上一条兜底 | 60s 后被强制关闭 |

## 标定步骤（首次使用必做）

`main/main.c` 顶部需要填两个标定值：

```c
#define SOIL_ADC_DRY        ???     /* 探头悬空（空气）实测 adc */
#define SOIL_ADC_WET        ???     /* 探头完全浸水实测 adc */
```

实测方法：

1. **烧入固件**（先用默认值），运行后串口会每秒打印一行 `soil: adc=XXXX  pct=YY%`
2. **测「干」值**：探头完全悬空（不接触任何东西），等 5 秒读数稳定，记下 adc 值
3. **测「湿」值**：拿一杯清水，探头两个金属片都没入水下，等 5 秒稳定，记下 adc 值
4. **修改代码**：`SOIL_ADC_DRY = 干值`、`SOIL_ADC_WET = 湿值`
5. **重新编译烧录**

典型值参考（仅供参考，**必须自己实测**）：

| 状态 | 典型 adc 范围 |
|---|---|
| 悬空（空气） | 3500~4095 |
| 完全浸水 | 1200~1800 |

## 预期效果

### OLED 显示（4 行 × 8px）

```
Soil:  45 %          ← 土壤湿度百分比
#########-----------  ← 20 格 ASCII 进度条
Pump: OFF            ← 水泵状态
ADC: 2517            ← 原始 ADC 值（标定用）
```

传感器故障时：

```
!! SENSOR ERR !!
Check probe
Pump LOCKED OFF
ADC: 4095
```

### 串口日志

正常采样（每 2 秒一行）：

```
I (...) app: soil: adc=2517  pct= 45%  pump=OFF
```

跨阈值时（额外高亮日志）：

```
W (...) app: Pump ON (pct=27%, 阈值 on<30  off>=70)
W (...) app: Pump OFF (pct=72%, 阈值 on<30  off>=70)
```

安全机制触发时（红色错误日志）：

```
E (...) app: 传感器读数越界: adc=4095 (允许范围 200~4095), 判定为故障
E (...) app: 传感器故障 -> 水泵强制关闭
E (...) app: 水泵已连续运行 60000 ms (上限 60000 ms), 强制关闭
```

## 项目结构

```
00-basic-esp-32E/
├─ sdkconfig.defaults        # 持久化板级配置
├─ partitions-4Mib.csv       # 4MB 分区表
├─ components/
│  └─ ssd1306/               # OLED 驱动 (I2C + 帧缓冲 + 6x8 字库)
│     ├─ ssd1306.h / .c
│     └─ font6x8.h
└─ main/
   ├─ CMakeLists.txt         # REQUIRES ssd1306 esp_adc esp_driver_gpio
   └─ main.c                 # 自动浇花主程序 (ADC + 控制 + 显示)
```

## 常见问题

| 现象 | 处理 |
|---|---|
| OLED 不亮 | 确认地址 0x3C（串口 `I2C 总线扫描` 日志）；检查 SCK/SDA 接线 |
| I2C 扫描「未检测到任何设备」，4 根线万用表通断正常 | **优先尝试交换 SDA/SCL 两根线**（OLED SDA→P21、SCL→P22 接反是最常见原因）。还不行再：① 拔出 4 根线重插排除接触不良；② 用 GPIO 翻转测试固件确认扩展板丝印 `P21`/`P22` 是否对应 GPIO21/GPIO22 |
| OLED 花屏/上下颠倒 | 调 `ssd1306.c` 中 `0xC8`↔`0xC0`、`0xA1`↔`0xA0`；SH1106 芯片将 `SSD1306_COL_OFFSET` 改为 2 |
| 土壤湿度 adc 一直 4095 | ① 传感器 VCC 没接 P18 / GPIO18 没配置；② 探头没接 AO；③ 用万用表量 P18 跟传感器 VCC 是否通 |
| 土壤湿度 adc 在水里不稳定（缓慢上升） | 探针被电化学腐蚀，800~1000 目砂纸轻轻打磨去氧化层。或确认 VCC 已改成 GPIO18 控制（不要直连 3V3） |
| 水泵一直开，关不掉 | 立刻断电！可能原因：① adc=4095（传感器断路/拔出），应触发 SENSOR ERR；② 阈值/标定值不对，pct 卡在 0%。看串口 E/W 日志确认 |
| 水泵运行 60s 就被强制关闭 | 正常安全机制，说明土壤没吸水或传感器读数没变。检查水泵实际是否出水、土壤是否真的湿 |
| 继电器没有「咔哒」声 | VCC 没接 5V / IN 没接 P5。万用表量模块 VCC-GND 应有 ~5V |
| 烧录 `chip stopped responding` | 关闭串口监视器；降波特率 `-b 115200`；必要时按住 BOOT 键再烧 |
| 构建报 `driver/gpio.h: No such file or directory` | ESP-IDF v6 把 `driver/*.h` 拆成 `esp_driver_*` 组件。在 `main/CMakeLists.txt` 的 REQUIRES 加 `esp_driver_gpio` |
| 运行 ~10 秒后 `stack overflow in task main`, 死循环前几次看起来正常 | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 被老项目继承到了 3584。`app_main` 里同时跑 WiFi 初始化 + 带 `ESP_LOGI` 的循环, 3.5KB 栈不够。在 `sdkconfig.defaults` 改成 `16384`, 删除 `sdkconfig` 后 `idf.py reconfigure` |
---

## 远程监控子系统 (新增)

ESP32 通过 WiFi + HTTPS 把读数推到 `afl.cn/plant/`，任何浏览器都能看实时数据。

### 架构

```
[ESP32]  --WiFi/HTTPS-->  [Hostinger Web App]  --pg-->  [Supabase Postgres]
   土壤传感器             Node.js Express           afl.cn/api/ingest
   继电器+OLED            后端服务 (server.js)      afl.cn/plant/ (前端)
```

### 关键文件

| 文件 | 作用 |
|---|---|
| `main/remote.c` | WiFi 连接 + HTTPS POST（包含 USER CONFIG 区域填 WiFi 凭据和 API key） |
| `main/remote.h` | remote 模块的公开 API |
| `server/server.js` | Node.js + Express 后端，监听 `/api/ingest` 和 `/api/data` |
| `server/public/index.html` | `afl.cn/plant/` 前端（Chart.js 折线图） |
| `server/.env.example` | 后端环境变量模板 |
| `sql/schema.sql` | Supabase Postgres 表结构 |

### 部署

**Supabase 端**：
1. 注册 supabase.com → 创建项目
2. SQL Editor 跑 `sql/schema.sql`
3. Project Settings → Database → Connection string (Transaction pooler)

**Hostinger 端**：
1. hPanel → Web Apps → 从 GitHub 部署 `server/` 子目录
2. 设环境变量：`DATABASE_URL` + `API_KEY`（与 ESP32 的 `DEVICE_API_KEY` 一致）
3. 自动 redeploy

**ESP32 端**：
1. `main/remote.c` USER CONFIG 填：
   - `WIFI_SSID` / `WIFI_PASS`
   - `DEVICE_API_KEY`（与后端 `API_KEY` 一致）
2. `sdkconfig.defaults` 启用 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`（验证 Let's Encrypt）
3. 编译烧录

### 推送周期

默认 5 分钟（`PUSH_PERIOD_MS = 300000`），可调。

### 调试常见问题

| 现象 | 处理 |
|---|---|
| ESP32 串口 `esp-tls-mbedtls: No server verification option` | 启用 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` |
| ESP32 串口 `mbedtls_x509_crt_parse returned -0x002C` | 硬编码证书格式问题，改用 mbedTLS bundle |
| 浏览器 `afl.cn/plant/` 空白 | 检查 Supabase 表是否存在 + `SHOW TABLES` |
| 推送 5xx | 看 Hostinger Runtime logs 找 `[ingest] DB error` |

---

## OTA 远程升级 (新)

不用插 USB,浏览器点一下就把 ESP32 固件刷上。

### 工作流

```bash
# 1. 编译新固件
. /Users/jinqun/.espressif/v6.0.1/esp-idf/export.sh
idf.py build

# 2. 把 .bin 拷贝到 server/ 并 bump 版本号
mkdir -p server/public/firmware
cp build/00-basic.bin server/public/firmware/latest.bin
echo "1.0.1" > server/public/firmware/latest.version

# 3. bump main.c 里的 FW_VERSION (OTA 检查用)
sed -i '' 's/FW_VERSION = "1.0.0"/FW_VERSION = "1.0.1"/' main/main.c

# 4. commit + push (Hostinger 自动 redeploy,firmware 文件就上线)
git add -A
git commit -m "release: v1.0.1"
git push
```

### 用户触发

打开 `afl.cn/plant/`,点 "⬆️ 检查更新":

- 当前 = 云端 → "✅ 已是最新 (1.0.1)"
- 当前 < 云端 → 入队 OTA_UPDATE 命令 → ESP32 下次 push (≤5min) 时:
  - 拉 `/api/firmware/latest` → `esp_https_ota` 下载到 ota_1 分区
  - 重启 → bootloader 从 ota_1 启动 → 30s 内没 crash 就 mark valid
  - ESP32 跑新固件, OLED 显示新版本号

### 安全机制

- **A/B 双分区**: ota_0 / ota_1 各 1MB,新固件写 ota_1,不覆盖当前在跑的
- **Bootloader 验证**: 启动新固件后 30s 内不 crash 才 mark valid
- **自动回滚**: 如果新固件 boot 失败, bootloader 自动切回 ota_0
- **下载中断**: 网络断了不会写 flash,旧固件继续跑

### ⚠️ 首次迁移

老固件是基于 `factory` 分区编译的。新分区表是 `ota_0` + `ota_1`, **老固件在新分区表上启动会找不到合法分区**。

**第一次**部署 OTA 后,需要 USB 烧录一次:

```bash
. /Users/jinqun/.espressif/v6.0.1/esp-idf/export.sh
idf.py -p /dev/cu.wchusbserial-XXXX flash
```

烧进去的是新分区表 + 新固件(写到 ota_0)。**之后所有更新走 OTA,不再需要 USB**。

### 故障排查

| 现象 | 处理 |
|---|---|
| ESP32 串口 `OTA: esp_https_ota_begin 失败` | 看错误码: `ESP_ERR_HTTP_...` 是网络/HTTPS 问题; `ESP_ERR_OTA_...` 是分区/镜像问题 |
| 新固件刷了但没生效 | 30s 内如果新固件反复 crash,bootloader 自动回 ota_0 — 看串口确认是不是 rollback 了 |
| 浏览器 "检查更新" 报 "无固件" | 服务端 `public/firmware/latest.bin` 或 `latest.version` 缺失 |
| OTA 命令一直不入队 | 浏览器按钮只入队命令,实际下载要等下次 push;推送周期可调 30s DEBUG 加速 |
