# 🖥️ Remote Controller v2.0

A secure, high-performance remote desktop application for Windows with end-to-end encryption.

![Version](https://img.shields.io/badge/version-2.0.0-blue)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

## ✨ Features

### Security
- **ECDH Key Exchange** - Secure key negotiation using Elliptic Curve Diffie-Hellman (P-256)
- **AES-256-GCM Encryption** - All data encrypted with authenticated encryption
- **Connection Approval** - Agent can accept/reject connection requests
- **IP Whitelisting** - Restrict access to specific IP addresses
- **Unique Device IDs** - Hardware-based identification

### Performance
- **JPEG Compression** - Efficient screen transfer with adjustable quality
- **Scalable Resolution** - 25%, 50%, 75%, or 100% capture quality
- **Adjustable FPS** - 1-60 frames per second
- **Low Latency** - Optimized TCP with Nagle disabled

### User Experience
- **Beautiful Console UI** - Modern, colorful interface
- **Contact Management** - Save and manage remote PC addresses
- **Auto-Scaling Window** - Viewer automatically resizes to fit content
- **Fullscreen Mode** - Press F11 for immersive viewing
- **Zoom Controls** - Ctrl+Plus/Minus to zoom, Ctrl+0 to reset

### Advanced
- **Unattended Access** - Allow connections without manual approval
- **Background Service** - Run as startup service for always-on access
- **Mouse Scroll Support** - Full mouse wheel functionality
- **Keyboard Passthrough** - All keyboard input forwarded to remote

## 📦 Components

| File | Description |
|------|-------------|
| `rc_agent.exe` | Run on the **remote PC** you want to control |
| `rc_viewer.exe` | Run on **your PC** to view and control remote |
| `rc_service.exe` | Optional background service for unattended access |

## 🚀 Quick Start

### Building

**Option 1: Visual Studio Developer Command Prompt**
```batch
build.bat
```

**Option 2: MinGW-w64**
```batch
build_mingw.bat
```

### Basic Usage

1. **On the remote PC (Agent)**:
   ```
   rc_agent.exe
   → Select "Quick Start" or configure settings
   → Note the IP address shown
   ```

2. **On your PC (Viewer)**:
   ```
   rc_viewer.exe
   → Select "Connect to Agent"
   → Enter the agent's IP address
   → Wait for agent to accept
   ```

### Command Line

```bash
# Quick connect to agent
rc_viewer.exe 192.168.1.100

# Quick connect with custom port
rc_viewer.exe 192.168.1.100 5001

# Run service in background
rc_service.exe --service
```

## ⚙️ Configuration

### Agent Settings
| Setting | Default | Description |
|---------|---------|-------------|
| Port | 5000 | Listening port |
| Capture Scale | 50% | Screen resolution scale |
| JPEG Quality | 75 | Compression quality (1-100) |
| Target FPS | 30 | Frames per second |
| Unattended | No | Auto-accept connections |

### Viewer Settings
| Setting | Default | Description |
|---------|---------|-------------|
| Viewer Name | "Viewer" | Your identification |
| Requested FPS | 30 | Preferred frame rate |
| JPEG Quality | 75 | Preferred quality |
| Auto-Scale | Yes | Auto-resize window |

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
│  Send       │──── Control Packets ─────>│  Execute    │
│  Commands   │<──── Video Frames ────────│  Capture    │
└─────────────┘                           └─────────────┘
```

## 📁 Data Files

| File | Location | Purpose |
|------|----------|---------|
| `contacts.dat` | Working directory | Saved IP addresses |
| `service_config.ini` | %LOCALAPPDATA%\RemoteController | Service settings |
| `allowed_ips.txt` | %LOCALAPPDATA%\RemoteController | IP whitelist |

## ⌨️ Keyboard Shortcuts

### Viewer Window
| Shortcut | Action |
|----------|--------|
| F11 | Toggle fullscreen |
| ESC | Exit fullscreen |
| Ctrl + Plus | Zoom in |
| Ctrl + Minus | Zoom out |
| Ctrl + 0 | Reset zoom (fit to window) |

## 🔧 Troubleshooting

### Connection Failed
1. Check firewall allows port 5000 (or configured port)
2. Verify IP address is correct
3. Ensure agent is running and listening

### Black Screen
1. Check JPEG quality is not too low
2. Verify screen capture permissions (run as admin if needed)
3. Check DPI awareness settings

### High Latency
1. Lower capture scale (25% or 50%)
2. Reduce JPEG quality
3. Decrease target FPS
4. Use wired connection instead of WiFi

### Encryption Errors
1. Ensure Windows version supports CNG (Vista+)
2. Check bcrypt.dll is present
3. Try running as administrator

## 📋 Requirements

- **OS**: Windows 7 or later (Windows 10+ recommended)
- **Compiler**: Visual Studio 2019+ or MinGW-w64
- **Libraries**: Windows SDK (included with Visual Studio)

## 🏗️ Project Structure

```
remote controller/
├── rc_common.h        # Shared protocol, encryption, UI
├── rc_agent.cpp       # Agent application
├── rc_viewer.cpp      # Viewer application
├── rc_service.cpp     # Background service
├── stb_image.h        # JPEG decoder
├── stb_image_write.h  # JPEG encoder
├── build.bat          # MSVC build script
├── build_mingw.bat    # MinGW build script
└── README.md          # This file
```

## 📄 License

You are free to use, modify, distribute, and adapt this work for educational purposes, academic submissions, or final-year projects, provided the original copyright and permission notice are preserved.

## 👤 Author

Created as a final year project demonstrating:
- Network programming with Winsock
- Windows GDI screen capture
- Modern cryptography (ECDH, AES-GCM)
- Multi-threaded application design
- User interface design

---

**⚠️ Disclaimer**: This software is intended for legitimate remote access to your own computers. Always obtain proper authorization before accessing any computer remotely.

