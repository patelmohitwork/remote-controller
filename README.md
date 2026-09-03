# 🖥️ Remote Controller

A secure, high-performance, native Win32 remote desktop application for Windows featuring pure GUI subsystems, 40 FPS low-latency video streaming, end-to-end encryption, Encrypted Live Text Chat, Dual-Pane Remote File Manager, and Windows UAC / Lock Screen elevation support.

![Version](https://img.shields.io/badge/version-2.5.0-blue)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

---

## ✨ Key Features

### 🚀 Performance & Video Streaming Engine
- **40 FPS Performance Engine** - High-precision 40 FPS locked rendering using `timeBeginPeriod(1)` OS timer resolution.
- **Fast GDI Capture (`COLORONCOLOR`)** - Optimized screen capture completing in <1ms per frame.
- **Dynamic Frame Differencing** - Skips unchanged frames to minimize CPU and network overhead.
- **Instant Repainting** - Immediate viewer canvas updates via `RedrawWindow`.

### 🛡️ Security & Authentication
- **ECDH Key Exchange** - Curve P-256 key negotiation for session key derivation.
- **AES-256-CBC Encryption** - All screen frames, input events, chat messages, and file operations are end-to-end encrypted.
- **Unattended Access Password** - Configurable password protection for unattended remote access.
- **Native GUI Connection Prompts** - Interactive `MessageBoxA` popups on the agent machine for one-click approval/rejection.
- **IP Whitelisting & Hardware IDs** - Restrict connections to specific IP addresses and track unique hardware device IDs.

### 💬 Encrypted Live Text Chat
- **Bi-Directional Messaging** - Real-time live text chat between Viewer and Agent.
- **Dedicated GUI Window** - Thread-safe GUI Chat dialogs (`WM_APP` message dispatching) with auto-open on incoming messages.
- **Clean UI** - Emoji-free, clean professional interface.

### 📂 Dual-Pane Graphical Remote File Manager
- **Side-by-Side Dual Pane Layout** - View Local Computer (Left Pane) and Remote Computer (Right Pane) files simultaneously.
- **Directory Navigation** - Double-click folders to navigate drives or use `[ Up ]` button.
- **File Transfer & Operations**:
  - `[ Upload >> ]` - Send selected local files to the remote PC folder.
  - `[ << Download ]` - Fetch selected remote files to the local PC folder.
  - `[ Delete Remote ]` - Remove remote files or directories.
  - `[ New Folder ]` - Create new directories on the remote host.

### 🔑 Windows UAC & Lock Screen Live Control
- **Real `Ctrl+Alt+Del` (SAS) Trigger** - 5-stage multi-strategy pipeline utilizing Windows `sas.dll` (`SendSAS`), Registry `SoftwareSASGeneration` policy configuration, and keyboard fallback.
- **Lock Screen Live Feed Streaming** - Dynamically attaches to `OpenInputDesktop` / `Winlogon` desktop context with `SRCCOPY | CAPTUREBLT` GDI capture and `CreateDCA("DISPLAY")` fallback. Stream and interact with the remote PC even when locked on the Windows logon screen or UAC elevation prompts.
- **Session Control Bar** - Instant action buttons at the top of the session window: **Chat**, **Files**, **Ctrl+Alt+Del**, **Lock PC**, and **TaskMgr**.

### 🎨 Pure Win32 GUI Subsystem (`-mwindows`)
- **Zero Command Prompt Windows** - Compiles under `-mwindows` / `/SUBSYSTEM:WINDOWS`. Both Agent and Viewer run as native Win32 GUI applications.
- **Graphical Connection Launcher** - Viewer dialog with Saved Contacts list, IP/Port fields, Password field, and Quick Connect button.
- **Agent Control Dashboard** - Agent dashboard displaying Port, Quality Scale, Unattended Toggle & Password, Status Badge, Live Chat button, and Disconnect button.

---

## 📦 Components

| Executable | Description |
|------------|-------------|
| `rc_agent.exe` | Native GUI server app running on the **remote PC** to share screen and accept connections |
| `rc_viewer.exe` | Native GUI client app running on **your PC** to view, control, chat, and transfer files |
| `rc_service.exe` | Background service monitor to auto-start agent and handle unattended startup |

---

## 🚀 Quick Start

### Building from Source

**Option 1: MinGW-w64 (Recommended)**
```cmd
build_mingw.bat
```

**Option 2: Visual Studio Developer Command Prompt (MSVC)**
```cmd
build.bat
```

### Basic Usage

1. **On the Remote Computer (`rc_agent.exe`)**:
   - Double-click `rc_agent.exe` to launch the **Agent Control Dashboard**.
   - Configure Port (default: `5000`), Quality Scale, and optional Unattended Access Password.
   - Click **Start Agent Server**.

2. **On Your Local Computer (`rc_viewer.exe`)**:
   - Double-click `rc_viewer.exe` to launch the **GUI Connection Launcher**.
   - Enter the Remote PC's IP address, Port, and Password (if unattended access is enabled).
   - Click **Connect to Agent**.

---

## ⚙️ Configuration Parameters

### Agent Dashboard Options
| Parameter | Default | Description |
|-----------|---------|-------------|
| Port | `5000` | TCP listening port |
| Quality Scale | Medium (50%) | Screen capture scale factor (25%, 50%, 75%, 100%) |
| Unattended Access | Disabled | Auto-accept connections with password verification |
| Unattended Password | None | Secret passphrase required for unattended connection |

### Viewer Settings
| Parameter | Default | Description |
|-----------|---------|-------------|
| Target FPS | 40 FPS | Locked frame rate for smooth remote viewing |
| JPEG Quality | 75 | Image compression quality (1-100) |
| Connect Password | None | Password sent during handshake for unattended access |

---

## 🔒 Security Architecture

```
┌─────────────┐                           ┌─────────────┐
│   VIEWER    │                           │    AGENT    │
├─────────────┤                           ├─────────────┤
│ Generate    │──── Handshake Init ──────>│             │
│ ECDH Keys   │                           │ Generate    │
│             │<─── Agent Public Key ─────│ ECDH Keys   │
│             │──── Viewer Public Key ───>│             │
│             │                           │             │
│ Derive      │                           │ Derive      │
│ AES-256 Key │                           │ AES-256 Key │
│             │<─── Handshake OK ─────────│             │
├─────────────┤                           ├─────────────┤
│             │═══ Encrypted Channel ═════│             │
│  Send       │──── Control & Commands ──>│  Execute    │
│  Packets    │<─── Video & Responses ────│  Capture    │
└─────────────┘                           └─────────────┘
```

---

## ⌨️ Keyboard Shortcuts & Controls

### Viewer Session Window
| Control / Button | Action |
|------------------|--------|
| `Chat` Button | Open Encrypted Live Text Chat window |
| `Files` Button | Open Dual-Pane Remote File Manager window |
| `Ctrl+Alt+Del` Button | Issue Secure Attention Sequence (`SendSAS`) |
| `Lock PC` Button | Instantly lock remote workstation (`Win+L`) |
| `TaskMgr` Button | Launch Windows Task Manager (`taskmgr.exe`) |
| `F11` | Toggle Fullscreen mode |
| `ESC` | Exit Fullscreen mode |
| Drag & Drop Files | Drag files onto session window to upload |

---

## 📋 Requirements

- **OS**: Windows 7 / 8.1 / 10 / 11 (Windows 10/11 recommended)
- **Compiler**: MinGW-w64 (GCC 6.3+) or Visual Studio 2019+
- **Libraries**: Winsock (`Ws2_32`), CNG (`Bcrypt`), GDI (`Gdi32`), Winmm (`Winmm`)

---

## 📄 License

You are free to use, modify, distribute, and adapt this work for educational purposes, academic submissions, or final-year projects, provided the original copyright and permission notice are preserved.

## 👤 Author & Support

If you run into any issues or have questions, feel free to reach out at **mohit@mohitpatel.work**.

---

**⚠️ Disclaimer**: This software is intended for legitimate remote administration of your own computers. Always obtain proper authorization before accessing any computer remotely.

