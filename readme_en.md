# P2PDesk

A low-latency P2P screen casting and remote control application built with C++, Qt6, and FFmpeg.

## Features

- **H.265 Video Streaming** — Real-time screen capture encoded with HEVC via FFmpeg
- **Audio Streaming** — Captures and streams system audio (ALSA)
- **Remote Control** — Mouse movement, click, scroll, and keyboard input over TCP
- **Qt6 GUI** — Separate server and client GUIs with a unified launcher
- **UDP + TCP Protocol** — UDP for media (video/audio), TCP for control messages

## Architecture

| Component | Description |
|-----------|-------------|
| **Server** | Captures screen (X11 + XTest) and audio (ALSA), encodes with H.265, streams over UDP |
| **Client** | Receives UDP stream, decodes with FFmpeg, renders in Qt widget, sends control input via TCP |
| **Protocol** | Custom binary protocol with magic-byte headers, UDP fragmentation, and TCP control messages |

## Dependencies

- **Qt6** — Core, Gui, Widgets, Network, Multimedia
- **FFmpeg** — libavcodec, libavformat, libavutil, libswscale
- **X11** — libX11, libXtst (screen capture & input simulation)
- **ALSA** — libasound (audio capture)
- **Vulkan** — GPU-accelerated rendering

### Ubuntu/Debian

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-multimedia-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libx11-dev libxtst-dev libasound2-dev
```

## Build

### CMake (Recommended)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Make

```bash
make
```

## Usage

```bash
# Unified launcher (choose server or client mode)
./screencast

# Or build and run separately
./server   # Start the server to share screen
./client   # Connect to a server
```

## Project Structure

```
P2PDesk/
├── main.cpp                # Unified launcher entry point
├── server.cpp              # Standalone server CLI
├── client.cpp              # Standalone client CLI
├── CMakeLists.txt          # CMake build config
├── Makefile                # Make build config
├── common/
│   ├── protocol.h          # Shared binary protocol definitions
│   └── H265Codec.*         # H.265 encoder/decoder wrapper
├── server_gui/
│   ├── main.cpp            # Server GUI entry
│   ├── MainWindow.*        # Server main window
│   └── ServerComponents.*  # Screen/audio capture components
└── client_gui/
    ├── main.cpp            # Client GUI entry
    ├── MainWindow.*        # Client main window
    └── ClientComponents.*  # Stream receiving/rendering components
```
