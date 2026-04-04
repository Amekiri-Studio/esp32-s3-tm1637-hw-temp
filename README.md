# Demo1

这个项目现在的功能是：

- `ESP32-S3` 通过串口接收 Linux 主机发送的 CPU 温度
- `TM1637` 四位数码管显示收到的整数温度
- 串口监视器输出最近一次收到的温度值

## 固件行为

固件代码位于 [src/main.cpp](src/main.cpp)。

- 启动后，数码管先显示 `----`
- 当 Linux 主机通过串口发送一行温度值，例如 `48.25` 时，ESP32-S3 会解析该值并显示四舍五入后的整数
- 如果 5 秒内没有收到新数据，数码管会重新显示 `----`

## 主机发送脚本

脚本位于 [tools/send_cpu_temp.py](tools/send_cpu_temp.py)。

脚本现在已经重构成跨平台骨架：

- 统一入口是 `tools/send_cpu_temp.py`
- Linux 温度读取已实现
- Windows 已接入 `LibreHardwareMonitor`
- macOS 的 provider 仍是骨架，后续需要接入自己的传感器后端
- 串口发送层是共用的

### 当前平台状态

- Linux: 可直接使用，优先从 `/sys/class/hwmon` 查找常见 CPU 温度传感器，找不到时回退到 `/sys/class/thermal`
- Windows: 可通过 `LibreHardwareMonitorLib.dll` 读取 CPU 温度
- macOS: 已有骨架，待接入具体温度来源

### 查看检测到的传感器

```bash
python3 tools/send_cpu_temp.py --list-sensors
```

### 只在终端打印温度，不发送到串口

```bash
python3 tools/send_cpu_temp.py --dry-run
```

### 发送温度到 ESP32-S3

```bash
python3 tools/send_cpu_temp.py --port /dev/ttyACM0
```

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
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll" --dry-run
py tools/send_cpu_temp.py --platform windows --librehardwaremonitor-dll "C:\path\to\LibreHardwareMonitorLib.dll" --port COM3
```

也可以通过环境变量指定 DLL：

```powershell
$env:LIBREHARDWAREMONITOR_DLL="C:\path\to\LibreHardwareMonitorLib.dll"
py tools/send_cpu_temp.py --platform windows --list-sensors
```

### 手动指定传感器路径

如果自动识别没有选中你想要的 CPU 传感器，可以手动指定：

```bash
python3 tools/send_cpu_temp.py \
  --sensor /sys/class/hwmon/hwmon0/temp1_input \
  --port /dev/ttyACM0
```

## 骨架结构

- [tools/send_cpu_temp.py](tools/send_cpu_temp.py): 统一 CLI 入口
- [tools/host_temp/cli.py](tools/host_temp/cli.py): 参数解析与主流程
- [tools/host_temp/serial_io.py](tools/host_temp/serial_io.py): 共用串口发送层
- [tools/host_temp/providers/linux.py](tools/host_temp/providers/linux.py): Linux 传感器实现
- [tools/host_temp/providers/windows.py](tools/host_temp/providers/windows.py): Windows LibreHardwareMonitor provider
- [tools/host_temp/providers/macos.py](tools/host_temp/providers/macos.py): macOS provider 骨架
