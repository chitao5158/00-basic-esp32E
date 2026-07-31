# dns_server

**这不是本项目自己写的代码。** 原样复制自 ESP-IDF 官方示例:

```
$IDF_PATH/examples/protocols/http_server/captive_portal/components/dns_server/
```

- ESP-IDF 版本: **v6.0.1**
- 授权: `Unlicense OR CC0-1.0` (SPDX 头保留在 `dns_server.c` / `dns_server.h` 里)
- 版权: Espressif Systems (Shanghai) CO LTD

## 用途

Captive Portal 的 DNS 诱捕:绑 UDP:53,把**所有** A 查询都回成 SoftAP 的 IP
(192.168.4.1),这样手机的联网探测请求会被引到我们的 HTTP server,触发
「需要登录」弹窗。

## 用法

```c
#include "dns_server.h"

dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
dns_server_handle_t h = start_dns_server(&cfg);
/* ... */
stop_dns_server(h);
```

`"*"` 是组件支持的通配名(见 `dns_server.c` 里 `strcmp(entry.name, "*")` 那一行),
表示不管查什么域名都用同一条规则回答。`"WIFI_AP_DEF"` 是 SoftAP netif 的 key,
组件会动态取该 netif 当前的 IP 来回答,所以不用硬编码 IP。

## 升级注意

若将来升级 ESP-IDF,建议重新从新版本 example 复制一次,以跟上上游修复。
