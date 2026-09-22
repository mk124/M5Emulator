<div align="center">
  <img src="resources/AppIcon.png" alt="M5 Emulator 应用图标" width="128" height="128">
  <h1>M5 Emulator</h1>
  <p>基于 QEMU 的非官方 M5Stack 设备模拟器，用于固件开发和测试。</p>
  <p>
    <a href="https://github.com/mk124/M5Emulator/releases/latest"><img src="https://img.shields.io/github/v/release/mk124/M5Emulator?label=version" alt="最新版本"></a>
    <a href="https://github.com/mk124/M5Emulator/actions/workflows/build.yml"><img src="https://github.com/mk124/M5Emulator/actions/workflows/build.yml/badge.svg?event=push" alt="发布构建状态"></a>
  </p>
  <p><a href="README.md">English</a> | <strong>中文</strong></p>
</div>

目前支持 **M5Stack StopWatch**。

项目基于 C++26、SDL2 和 Espressif QEMU，**可直接运行未经修改的原始 BIN 固件**：固件中的 Xtensa 指令在模拟的 ESP32-S3 CPU 上执行，无需将固件移植或重新编译为桌面程序。

模拟器提供显示、触控、按键、传感器和音频模拟，并通过宿主机桥接蓝牙通信。

## 运行

打开 **M5 Emulator.app**，将 `.bin` 文件拖入窗口即可，命令行参数可选。

<p align="center">
  <img src="resources/screenshot.png" alt="运行 StopWatch UserDemo 固件" width="420">
</p>

模拟 StopWatch 时，可载入 ESP32-S3 应用固件（BIN 文件），也可载入完整的 16 MiB Flash 镜像。

应用 BIN 使用随附的[引导程序](resources/esp32s3/README.md)和[默认分区表](src/models/stopwatch/StopWatchFlash.hpp)，放置于 `0x20000`，最大支持 `0x4F0000` 字节；未使用的 Flash 区域填充为 `0xFF`。完整 Flash 镜像保留自身的引导程序、分区和数据。应用必须与所选设备和 Flash 布局兼容。

模拟器使用独立的 Flash 存档，不会修改原始 BIN。使用 `--no-persist` 可关闭持久化，不保存运行期间的 Flash 改动。`--state-flash FILE` 可指定存档：文件不存在时从 BIN 创建，已存在时直接使用，不会从 BIN 重新初始化。

## 存储与日志

在 macOS 上，应用数据保存在 `~/Library/Application Support/M5Emulator/`：

- `devices/<设备 ID>/flash/`：该设备的 Flash 存档。
- `devices/<设备 ID>/recent-files.txt`：该设备的最近文件列表。
- `logs/`：每次运行的 stdout 和 stderr 日志，同时保留控制台输出。

截图和录像默认保存在 `~/Pictures/M5 Emulator/`，可通过 `--capture-dir DIR` 指定其他目录。

## 硬件与兼容性

| 组件 | 对应设备 | 已实现范围 |
|---|---|---|
| ESP32-S3 | StopWatch | QEMU CPU、Flash、PSRAM、GPIO、SPI/GDMA、I²C、基础 ADC，以及支持定时器/EXT1 唤醒的 Light-sleep |
| CO5300 | StopWatch | 466×466 显示、RGB565 更新、屏幕电源/复位、TE 和可选亮度模拟 |
| CST820 | StopWatch | 单点触控、触控变化中断及复位/休眠行为 |
| BMI270 | StopWatch | 六轴输入、量程和采样配置、数据就绪中断及近似的任意运动中断 |
| RX8130CE | StopWatch | 日历、时间设置和闹钟中断 |
| M5PM1 / M5IOE1 | StopWatch | 固定电源读数、电源键中断、外设电源/复位、LED 和振动反馈 |
| ES8311 / I²S | StopWatch | 扬声器和麦克风 PCM、音量/静音及 DMA |
| BLE | StopWatch | 虚拟控制器、ATT/GATT/SMP 对端和 macOS 外设桥接 |

BLE 通过受支持的 SDK 库特征和 ABI 进行识别，不依赖应用名称或固定的应用地址。不需要 ELF/MAP 文件，也不会修改 BIN 内容或客体代码指令。未知库版本、无法唯一确定的匹配和未支持的功能仍可能导致某些固件无法使用 BLE。

桥接支持单个客户端会话，虚拟链路使用常规 ATT 和 Secure Connections Just Works。macOS 独立管理物理链路及其安全状态。CoreBluetooth 对服务发布、描述符和连接控制存在限制，因此不保证完整的 GATT、HID、配对及无线 OTA 兼容性。不支持同一台 Mac 上的蓝牙自连接，无线测试请使用另一台设备。

尚未实现 Wi-Fi、完整 USB 功能、Deep-sleep、完整 IMU 手势处理和物理射频行为。客体报告的时钟设置不代表等效的实际 CPU 吞吐量。Windows 蓝牙及进程/传输后端尚未实现。

## 构建

目前支持在 macOS 上构建。请安装 Xcode 命令行工具、支持 C++26 的编译器、CMake 3.30 或更新版本、Ninja、Python 3、pkg-config、SDL2、SDL3、GLib、Pixman 和 libgcrypt。首次构建需要联网。

```sh
bash tools/build.sh
ctest --test-dir build --output-on-failure
```

完成本机架构构建后，运行 `bash tools/build_intel.sh` 构建 Intel 版本。

将 Apple Silicon 和 Intel 版本打包为 `dist/M5 Emulator.app`：

```sh
python3 tools/package_macos.py
```

当前安装包的最低系统要求为：Apple Silicon 版 macOS 26，Intel 版 macOS 11。

## CLI 参考

| 参数 | 用途 |
|---|---|
| `--flash FILE.bin` | 载入固件 |
| `--device ID` | 选择设备（默认：`stopwatch`） |
| `--no-audio` | 关闭扬声器和麦克风 |
| `--[no-]mic` | 麦克风输入 |
| `--[no-]bluetooth` | 宿主 BLE 桥接 |
| `--[no-]persist` | Flash 持久化 |
| `--[no-]icount` | 指令计数定时 |
| `--[no-]brightness` | 屏幕亮度模拟 |
| `--state-flash FILE` | 指定 Flash 存档 |
| `--capture-dir DIR` | 截图和录像目录 |
| `--record FILE.mp4` | 录制画面和声音 |
| `--screenshot FILE.bmp` | 退出时保存截图 |
| `--headless` | 无窗口运行 |
| `--seconds N` | N 秒后退出 |
| `--qemu PATH` | 指定 QEMU 可执行文件 |
| `--ble-peer SOCKET` | 使用外部 BLE 协议对端 |
| `--[no-]ble-hle` | 蓝牙控制器接管 |
| `-h`, `--help` | 显示帮助 |

`--[no-]…` 表示启用／禁用参数，例如 `--mic` / `--no-mic`。

限时无窗口运行示例：

```sh
./build/m5-emulator --flash /path/to/firmware.bin \
    --headless --no-audio --no-bluetooth --no-persist \
    --seconds 20 --screenshot /tmp/screen.bmp
```

## 许可证

本项目自有代码采用 **GPL-3.0-or-later**，详见 [LICENSE](LICENSE)。第三方组件保留各自的许可条款和声明。随附引导程序和 BLE 数据的声明见 [resources/esp32s3/LICENSE](resources/esp32s3/LICENSE)，字体署名见 [resources/fonts/README.md](resources/fonts/README.md)。
