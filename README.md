# P2PDesk

一款基于 C++、Qt6 和 FFmpeg 的低延迟 P2P 屏幕投射与远控应用。

## 功能特性

- **H.265 视频流** — 实时屏幕捕获，通过 FFmpeg 以 HEVC 编码传输
- **音频流** — 捕获并传输系统音频（ALSA）
- **远控功能** — 通过 TCP 传输鼠标移动、点击、滚轮及键盘输入
- **Qt6 图形界面** — 服务端与客户端独立 GUI，统一启动器
- **UDP + TCP 双协议** — UDP 传输音视频，TCP 传输控制信令

## 架构设计

| 组件 | 说明 |
|------|------|
| **服务端 (Server)** | 捕获屏幕（X11 + XTest）和音频（ALSA），H.265 编码后通过 UDP 发送 |
| **客户端 (Client)** | 接收 UDP 流，FFmpeg 解码，Qt 控件渲染，通过 TCP 发送控制指令 |
| **通信协议** | 自定义二进制协议，魔数校验头、UDP 分片、TCP 控制消息 |

## 依赖

- **Qt6** — Core, Gui, Widgets, Network, Multimedia
- **FFmpeg** — libavcodec, libavformat, libavutil, libswscale
- **X11** — libX11, libXtst（屏幕捕获与输入模拟）
- **ALSA** — libasound（音频捕获）
- **Vulkan** — GPU 加速渲染

### Ubuntu/Debian 安装

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-multimedia-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libx11-dev libxtst-dev libasound2-dev
```

## 编译

### CMake（推荐）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Make

```bash
make
```

## 运行

```bash
# 统一启动器（可选择服务端或客户端模式）
./screencast

# 或分别运行
./server   # 启动服务端，共享屏幕
./client   # 连接服务端
```

## 项目结构

```
P2PDesk/
├── main.cpp                # 统一启动器入口
├── server.cpp              # 服务端命令行版本
├── client.cpp              # 客户端命令行版本
├── CMakeLists.txt          # CMake 构建配置
├── Makefile                # Make 构建配置
├── common/
│   ├── protocol.h          # 共享二进制协议定义
│   └── H265Codec.*         # H.265 编解码封装
├── server_gui/
│   ├── main.cpp            # 服务端 GUI 入口
│   ├── MainWindow.*        # 服务端主窗口
│   └── ServerComponents.*  # 屏幕/音频捕获组件
└── client_gui/
    ├── main.cpp            # 客户端 GUI 入口
    ├── MainWindow.*        # 客户端主窗口
    └── ClientComponents.*  # 流接收/渲染组件
```
