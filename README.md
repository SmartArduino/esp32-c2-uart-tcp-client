# ESP32-C2 UART TCP 透传固件

该工程基于 ESP-IDF，满足以下流程：

1. 设备首次上电进入 AP 配网模式，提供 HTTP 配网页面配置连接路由器的 SSID/密码。
2. 按住按键 3 秒恢复出厂设置并重新进入 AP 配网模式。
3. 成功连接路由后启动 UDP 发现服务，局域网设备可通过广播获取 IP。
4. 启动 TCP Server，将 UART 数据透传到已连接的 TCP Client（并支持反向写入串口）。

## 默认参数

- AP SSID：`ESP32C2-SETUP`
- AP 密码：`12345678`
- 配网地址：`http://192.168.4.1/`
- UDP 发现端口：`3333`（发送 `DISCOVER`，回应 `IP:<addr>`）
- TCP 透传端口：`12345`
- 恢复出厂按键：`GPIO0`
- UART：`UART1`，TX=`GPIO4`，RX=`GPIO5`，波特率 `115200`

## 使用方式

1. 设备上电后，手机/电脑连接 AP，打开 `http://192.168.4.1/` 配置路由 SSID/密码。
2. 设备保存配置后重启，连接到路由器。
3. 局域网内设备发送 UDP 广播：
   ```text
   DISCOVER
   ```
   设备会回复 `IP:<addr>`，然后使用该 IP 连接 TCP 端口 `12345`。
4. 串口数据会实时透传给 TCP Client。

## 构建与烧录

```bash
idf.py set-target esp32c2
idf.py build flash monitor
```

如需修改引脚或端口，直接编辑 `main/main.c` 中的宏定义。
