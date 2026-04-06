# 基于ESP32-S3和TM1637的电脑硬件温度显示器（CPU温度）

这个项目现在的功能是：

- `ESP32-S3` 通过串口接收主机发送的 CPU 温度
- `ESP32-S3` 也可以通过串口接收 WiFi 配置命令并连接到局域网
- `ESP32-S3` 在连上 WiFi 后，也可以通过局域网 UDP 接收主机发送的 CPU 温度
- `TM1637` 四位数码管显示收到的整数温度
- 串口监视器输出最近一次收到的温度值

## 固件行为

固件代码位于 [src/main.cpp](src/main.cpp)。

- 启动后，数码管先显示 `----`
- 当主机通过串口发送一行温度值，例如 `48.25` 时，ESP32-S3 会解析该值并显示四舍五入后的整数
- 当主机通过 WiFi UDP 发送一条温度文本，例如 `48.25` 时，ESP32-S3 也会解析并显示四舍五入后的整数
- 当串口和 WiFi 都在发送温度时，显示优先级是“串口优先，WiFi 备用”
- 只要最近 5 秒内收到过串口温度，就持续显示串口温度；串口超时后，才会回退显示最近的 WiFi 温度
- WiFi UDP 启用了“发送端独占锁”：第一个发来有效温度的主机拿到 owner 锁，锁有效期间只接受这个 IP 的温度
- 如果当前 WiFi owner 超过 5 秒没有再发送有效温度，owner 锁会自动释放，下一台主机才可以接管
- 当通过串口完成 WiFi 连接后，数码管会短暂显示 `AAAA`
- 如果当前已经连上 WiFi，但 5 秒内没有收到新温度，数码管会显示 `AAAA`
- 如果 5 秒内没有收到新数据且 WiFi 未连接，数码管会显示 `----`
- WiFi 连接成功后，会把当前 SSID/密码保存到 ESP32 闪存
- 下次上电启动时，会自动读取上一次成功连接的 WiFi 并尝试重连
- WiFi 温度接收默认使用 UDP 端口 `4210`

## WiFi 串口配置

WiFi 配置入口已经接入固件，当前仍然保留原来的串口温度输入能力。也就是说：

- 温度数据仍然可以继续按原样通过串口发送
- WiFi 连接也通过同一个串口完成配置
- 当前已经接入“主机通过 WiFi UDP 发送温度数据”
- 也可以直接用 Python 脚本通过串口配置 WiFi，而不必手动打开串口监视器

可以在串口监视器里发送以下命令，每条命令一行：

```text
WIFI HELP
WIFI SSID YourWiFiName
WIFI PASS YourWiFiPassword
WIFI CONNECT
WIFI STATUS
WIFI LOCK CLEAR
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
- `WIFI CONNECT`：开始连接；连接成功后会自动写入闪存
- `WIFI STATUS`：查看当前 WiFi 状态、IP、是否已保存到闪存，以及 UDP 端口
- `WIFI STATUS` 也会显示当前 WiFi sender lock 是否存在，以及 owner IP
- `WIFI LOCK CLEAR`：手动释放当前 WiFi sender lock，让下一台主机可以立即接管
- `WIFI CLEAR`：清除当前保存的 SSID/密码，删除闪存记录，并断开 WiFi

### 通过 Python 脚本配置 WiFi

如果你不想手动打开串口监视器，也可以直接用 Python 脚本通过串口发送这些 WiFi 命令。

设置 SSID、密码并立即连接：

```bash
python3 tools/send_cpu_temp.py \
  --wifi-ssid YourWiFiName \
  --wifi-pass YourWiFiPassword \
  --wifi-connect
```

查看当前 WiFi 状态：

```bash
python3 tools/send_cpu_temp.py --wifi-status
```

这个命令会在发送 `WIFI STATUS` 后继续读取串口返回值，因此如果已经连网，终端里会直接看到 `WiFi IP: ...`。

清除已保存的 WiFi：

```bash
python3 tools/send_cpu_temp.py --wifi-clear
```

手动释放当前 WiFi sender lock：

```bash
python3 tools/send_cpu_temp.py --wifi-lock-clear
```

如果自动识别串口不准，也可以手动指定：

```bash
python3 tools/send_cpu_temp.py \
  --port /dev/ttyACM0 \
  --wifi-ssid YourWiFiName \
  --wifi-pass YourWiFiPassword \
  --wifi-connect
```

## 主机发送脚本

脚本位于 [tools/send_cpu_temp.py](tools/send_cpu_temp.py)。

脚本现在已经重构成跨平台骨架：

- 统一入口是 `tools/send_cpu_temp.py`
- Linux 温度读取已实现
- Windows 已接入 `LibreHardwareMonitor`
- macOS 已接入 Apple Silicon 的 `macmon` CLI
- 支持串口发送和 WiFi UDP 发送两种方式
- 也支持通过串口发送 WiFi 管理命令

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

### 通过 WiFi 发送温度到 ESP32-S3

先在串口监视器中执行：

```text
WIFI STATUS
```

记下 ESP32-S3 的局域网 IP，然后在主机侧发送：

```bash
python3 tools/send_cpu_temp.py --transport wifi --wifi-host 192.168.1.123
```

如果你需要显式指定端口，也可以这样：

```bash
python3 tools/send_cpu_temp.py --transport wifi --wifi-host 192.168.1.123 --wifi-port 4210
```

### 指定平台 provider

```bash
python3 tools/send_cpu_temp.py --platform linux --port /dev/ttyACM0
```

通过 WiFi 发送时也可以和 provider 组合：

```bash
python3 tools/send_cpu_temp.py --platform linux --transport wifi --wifi-host 192.168.1.123
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
