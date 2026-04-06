# 基于ESP32-S3和TM1637的电脑硬件温度显示器（CPU温度）

这个项目现在的功能是：

- `ESP32-S3` 通过串口接收主机发送的 CPU 温度
- `ESP32-S3` 也可以通过串口接收 WiFi 配置命令并连接到局域网
- `TM1637` 四位数码管显示收到的整数温度
- 串口监视器输出最近一次收到的温度值

## 固件行为

固件代码位于 [src/main.cpp](src/main.cpp)。

- 启动后，数码管先显示 `----`
- 当主机通过串口发送一行温度值，例如 `48.25` 时，ESP32-S3 会解析该值并显示四舍五入后的整数
- 当通过串口完成 WiFi 连接后，数码管会短暂显示 `AAAA`
- 如果当前已经连上 WiFi，但 5 秒内没有收到新温度，数码管会显示 `AAAA`
- 如果 5 秒内没有收到新数据且 WiFi 未连接，数码管会显示 `----`

## WiFi 串口配置

WiFi 配置入口已经接入固件，当前仍然保留原来的串口温度输入能力。也就是说：

- 温度数据仍然可以继续按原样通过串口发送
- WiFi 连接也通过同一个串口完成配置
- 真正的“通过 WiFi 发送温度数据”可以在这个基础上继续接下一步，例如 UDP、TCP 或 HTTP

可以在串口监视器里发送以下命令，每条命令一行：

```text
WIFI HELP
WIFI SSID YourWiFiName
WIFI PASS YourWiFiPassword
WIFI CONNECT
WIFI STATUS
WIFI CLEAR
```

推荐流程：

```text
WIFI SSID YourWiFiName
WIFI PASS YourWiFiPassword
WIFI CONNECT
```

说明：

- `WIFI SSID ...`：设置 WiFi 名称
- `WIFI PASS ...`：设置 WiFi 密码
- `WIFI CONNECT`：开始连接
- `WIFI STATUS`：查看当前 WiFi 状态和 IP
- `WIFI CLEAR`：清除当前保存的 SSID/密码并断开 WiFi

## 主机发送脚本

脚本位于 [tools/send_cpu_temp.py](tools/send_cpu_temp.py)。

脚本现在已经重构成跨平台骨架：

- 统一入口是 `tools/send_cpu_temp.py`
- Linux 温度读取已实现
- Windows 已接入 `LibreHardwareMonitor`
- macOS 已接入 Apple Silicon 的 `macmon` CLI
- 串口发送层是共用的，并支持自动识别 ESP32-S3 串口

### 当前平台状态

- Linux: 可直接使用，优先从 `/sys/class/hwmon` 查找常见 CPU 温度传感器，找不到时回退到 `/sys/class/thermal`
- Windows: 可通过 `LibreHardwareMonitorLib.dll` 读取 CPU 温度
- macOS: Apple Silicon 可通过 `macmon` 读取 `cpu_temp_avg`

### 查看检测到的传感器

```bash
python3 tools/send_cpu_temp.py --list-sensors
```

### 查看检测到的串口

```bash
python3 tools/send_cpu_temp.py --list-ports
```

### 只在终端打印温度，不发送到串口

```bash
python3 tools/send_cpu_temp.py --dry-run
```

### 发送温度到 ESP32-S3

```bash
python3 tools/send_cpu_temp.py
```

默认会自动识别常见的 ESP32-S3 串口；如果同时存在多个不确定的串口，可以配合 `--list-ports` 查看后手动指定。

### 指定平台 provider

```bash
python3 tools/send_cpu_temp.py --platform linux --port /dev/ttyACM0
```

### Windows + LibreHardwareMonitor

Windows 端建议准备两样东西：

- `LibreHardwareMonitor` 发布包中的 `LibreHardwareMonitorLib.dll`
- Python 包 `pythonnet`

如果你需要通过串口发给 ESP32，在 Windows 上还需要安装 `pyserial`。

```powershell
py -m pip install pythonnet pyserial
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll" --list-sensors
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll" --list-ports
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll" --dry-run
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll"
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll" --port COM3
```

也可以通过环境变量指定 DLL：

```powershell
$env:LIBREHARDWAREMONITOR_DLL="C:\path\to\LibreHardwareMonitorLib.dll"
py tools/send_cpu_temp.py --platform windows --list-sensors
```

### macOS + macmon（Apple Silicon）

macOS provider 目前只针对 Apple Silicon，实现方式是调用 `macmon` CLI 读取 `cpu_temp_avg`。

建议先安装：

```bash
brew install macmon
```

然后可以这样用：

```bash
python3 tools/send_cpu_temp.py --platform macos --list-sensors
python3 tools/send_cpu_temp.py --platform macos --list-ports
python3 tools/send_cpu_temp.py --platform macos --dry-run
python3 tools/send_cpu_temp.py --platform macos
python3 tools/send_cpu_temp.py --platform macos --port /dev/tty.usbmodem1101
```

如果 `macmon` 不在 `PATH` 里，也可以通过环境变量指定：

```bash
export MACMON_BIN="/opt/homebrew/bin/macmon"
python3 tools/send_cpu_temp.py --platform macos --dry-run
```

### 手动指定传感器路径

如果自动识别没有选中你想要的 CPU 传感器，可以手动指定：

```bash
python3 tools/send_cpu_temp.py \
  --sensor /sys/class/hwmon/hwmon0/temp1_input \
  --port /dev/ttyACM0
```

macOS Apple Silicon 默认会选 `cpu_temp_avg`，也可以显式指定：

```bash
python3 tools/send_cpu_temp.py --platform macos --sensor cpu_temp_avg --dry-run
```

## 骨架结构

- [tools/send_cpu_temp.py](tools/send_cpu_temp.py): 统一 CLI 入口
- [tools/host_temp/cli.py](tools/host_temp/cli.py): 参数解析与主流程
- [tools/host_temp/serial_io.py](tools/host_temp/serial_io.py): 共用串口发送层
- [tools/host_temp/providers/linux.py](tools/host_temp/providers/linux.py): Linux 传感器实现
- [tools/host_temp/providers/windows.py](tools/host_temp/providers/windows.py): Windows LibreHardwareMonitor provider
- [tools/host_temp/providers/macos.py](tools/host_temp/providers/macos.py): macOS Apple Silicon `macmon` provider
