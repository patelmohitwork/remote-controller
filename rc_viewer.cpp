/*
 * Remote Controller - Viewer Application
 * Connects to agent, receives encrypted frames, sends control commands
 * Features: Auto-scaling, contact management, secure handshake
 */

#include "rc_common.h"
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

// stb_image for JPEG decoding
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

struct ViewerConfig {
    std::string agentIP;
    uint16_t port = RC_DEFAULT_PORT;
    std::string viewerName = "Viewer";
    std::string password;
    int requestedFPS = 40;
    int jpegQuality = 75;
    bool autoScale = true;
    int zoomMode = 0;  // 0=fit, 1=100%, 2=custom
    float customScale = 1.0f;
    bool autoReconnect = true;
    int reconnectAttempts = 3;
    int reconnectDelayMs = 1500;
};

ViewerConfig g_viewerConfig;

class ViewerClient;
static ViewerClient* g_pViewerClient = nullptr;
static HWND g_hViewerChatWnd = nullptr;
static HWND g_hViewerChatList = nullptr;
void ShowViewerChatWindow(HWND parentHwnd);
void ShowFileManagerWindow(HWND parentHwnd);
void UpdateRemoteFileListInUI(const char* path, const FileItem* items, uint32_t count);
void SendSpecialKeyToActiveClient(uint8_t keyCmd);

// ============================================================================
// VIEWER WINDOW
// ============================================================================

#define WM_APP_NEWFRAME (WM_APP + 1)
#define WM_APP_RESIZE   (WM_APP + 2)
#define WM_APP_DISCONNECT (WM_APP + 3)

class ViewerWindow {
private:
    HWND hWnd = nullptr;
    CRITICAL_SECTION cs;
    std::vector<uint8_t> framePixels;
    BITMAPINFO bmpInfo{};
    int imgWidth = 0;
    int imgHeight = 0;
    float imgScale = 1.0f;
    bool hasFrame = false;
    SOCKET sock = INVALID_SOCKET;
    SecureChannel* crypto = nullptr;
    std::function<void(const std::vector<std::string>&)> dropHandler;
    uint32_t sequence = 0;

    // Window state
    int windowWidth = 1280;
    int windowHeight = 720;
    bool isFullscreen = false;
    RECT windowedRect{};
    LONG windowedStyle = 0;

public:
    ViewerWindow() {
        InitializeCriticalSection(&cs);
    }

    ~ViewerWindow() {
        DeleteCriticalSection(&cs);
    }

    void SetSocket(SOCKET s) { sock = s; }
    void SetCrypto(SecureChannel* c) { crypto = c; }
    void SetDropHandler(const std::function<void(const std::vector<std::string>&)>& handler) { dropHandler = handler; }
    HWND GetHWnd() const { return hWnd; }

    void UpdateFrame(const uint8_t* jpegData, size_t jpegSize, int w, int h, float scale) {
        // Decode JPEG
        int decW = 0, decH = 0, comp = 0;
        unsigned char* rgb = stbi_load_from_memory(jpegData, (int)jpegSize, &decW, &decH, &comp, 3);
        if (!rgb) return;

        EnterCriticalSection(&cs);

        imgWidth = decW;
        imgHeight = decH;
        imgScale = scale;

        // Convert RGB to BGRA for Windows
        framePixels.resize(decW * decH * 4);
        for (int i = 0; i < decW * decH; i++) {
            framePixels[i * 4 + 0] = rgb[i * 3 + 2];  // B
            framePixels[i * 4 + 1] = rgb[i * 3 + 1];  // G
            framePixels[i * 4 + 2] = rgb[i * 3 + 0];  // R
            framePixels[i * 4 + 3] = 255;             // A
        }

        ZeroMemory(&bmpInfo, sizeof(bmpInfo));
        bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmpInfo.bmiHeader.biWidth = decW;
        bmpInfo.bmiHeader.biHeight = -decH;  // Top-down
        bmpInfo.bmiHeader.biPlanes = 1;
        bmpInfo.bmiHeader.biBitCount = 32;
        bmpInfo.bmiHeader.biCompression = BI_RGB;

        hasFrame = true;
        LeaveCriticalSection(&cs);

        stbi_image_free(rgb);

        // Trigger immediate high-speed repaint
        if (hWnd) {
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

            // Auto-resize window to match content (first frame only or on size change)
            static int lastW = 0, lastH = 0;
            if (g_viewerConfig.autoScale && (decW != lastW || decH != lastH)) {
                lastW = decW;
                lastH = decH;
                PostMessage(hWnd, WM_APP_RESIZE, (WPARAM)decW, (LPARAM)decH);
            }
        }
    }

    void SendMouseEvent(uint8_t mouseType, int imgX, int imgY, int16_t scrollDelta = 0) {
        if (sock == INVALID_SOCKET || !crypto) return;

        ControlPacket pkt{};
        pkt.kind = 1;
        pkt.mouseType = mouseType;
        pkt.x = imgX;
        pkt.y = imgY;
        pkt.scrollDelta = scrollDelta;

        NetworkHelper::SendPacket(sock, MSG_CONTROL_MOUSE, &pkt, sizeof(pkt), crypto, sequence++);
    }

    void SendKeyEvent(uint16_t vk, bool down) {
        if (sock == INVALID_SOCKET || !crypto) return;

        ControlPacket pkt{};
        pkt.kind = 2;
        pkt.vk = vk;
        pkt.keyDown = down ? 1 : 0;

        NetworkHelper::SendPacket(sock, MSG_CONTROL_KEYBOARD, &pkt, sizeof(pkt), crypto, sequence++);
    }

    void ToggleFullscreen() {
        if (!isFullscreen) {
            // Save current window state
            GetWindowRect(hWnd, &windowedRect);
            windowedStyle = GetWindowLong(hWnd, GWL_STYLE);

            // Go fullscreen
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);

            SetWindowLong(hWnd, GWL_STYLE, windowedStyle & ~(WS_CAPTION | WS_THICKFRAME));
            SetWindowPos(hWnd, HWND_TOP,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            isFullscreen = true;
        } else {
            // Restore windowed mode
            SetWindowLong(hWnd, GWL_STYLE, windowedStyle);
            SetWindowPos(hWnd, nullptr,
                        windowedRect.left, windowedRect.top,
                        windowedRect.right - windowedRect.left,
                        windowedRect.bottom - windowedRect.top,
                        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            isFullscreen = false;
        }
    }

    double ComputeScale(int winW, int winH) {
        if (imgWidth <= 0 || imgHeight <= 0) return 1.0;

        switch (g_viewerConfig.zoomMode) {
            case 1: return 1.0;  // 100%
            case 2: return g_viewerConfig.customScale;  // Custom
            default: {  // Fit
                double sx = (double)winW / (double)imgWidth;
                double sy = (double)winH / (double)imgHeight;
                double s = (sx < sy) ? sx : sy;
                return (s <= 0.0) ? 1.0 : s;
            }
        }
    }

    void ConvertWindowToImageCoords(int winX, int winY, int& imgX, int& imgY) {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;

        double scale = ComputeScale(winW, winH);
        int destW = (int)(imgWidth * scale);
        int destH = (int)(imgHeight * scale);
        int destX = (winW - destW) / 2;
        int destY = (winH - destH) / 2;

        int localX = winX - destX;
        int localY = winY - destY;

        imgX = (int)(localX / scale);
        imgY = (int)(localY / scale);

        // Clamp
        if (imgX < 0) imgX = 0;
        if (imgY < 0) imgY = 0;
        if (imgX >= imgWidth) imgX = imgWidth - 1;
        if (imgY >= imgHeight) imgY = imgHeight - 1;
    }

    bool IsInsideImage(int winX, int winY) {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;

        double scale = ComputeScale(winW, winH);
        int destW = (int)(imgWidth * scale);
        int destH = (int)(imgHeight * scale);
        int destX = (winW - destW) / 2;
        int destY = (winH - destH) / 2;

        return (winX >= destX && winX < destX + destW &&
                winY >= destY && winY < destY + destH);
    }

#define IDC_VIEWER_BTN_CHAT    4001
#define IDC_VIEWER_BTN_FILES   4002
#define IDC_VIEWER_BTN_CAD     4003
#define IDC_VIEWER_BTN_LOCK    4004
#define IDC_VIEWER_BTN_TASKMGR 4005

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ViewerWindow* self = (ViewerWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            HWND hChat = CreateWindowA("BUTTON", "Chat", WS_CHILD | WS_VISIBLE, 10, 6, 75, 26, hwnd, (HMENU)IDC_VIEWER_BTN_CHAT, nullptr, nullptr);
            SendMessage(hChat, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hFiles = CreateWindowA("BUTTON", "Files", WS_CHILD | WS_VISIBLE, 90, 6, 75, 26, hwnd, (HMENU)IDC_VIEWER_BTN_FILES, nullptr, nullptr);
            SendMessage(hFiles, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hCad = CreateWindowA("BUTTON", "Ctrl+Alt+Del", WS_CHILD | WS_VISIBLE, 170, 6, 115, 26, hwnd, (HMENU)IDC_VIEWER_BTN_CAD, nullptr, nullptr);
            SendMessage(hCad, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hLock = CreateWindowA("BUTTON", "Lock PC", WS_CHILD | WS_VISIBLE, 290, 6, 85, 26, hwnd, (HMENU)IDC_VIEWER_BTN_LOCK, nullptr, nullptr);
            SendMessage(hLock, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hTaskMgr = CreateWindowA("BUTTON", "TaskMgr", WS_CHILD | WS_VISIBLE, 380, 6, 85, 26, hwnd, (HMENU)IDC_VIEWER_BTN_TASKMGR, nullptr, nullptr);
            SendMessage(hTaskMgr, WM_SETFONT, (WPARAM)hFont, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_VIEWER_BTN_CHAT) {
                ShowViewerChatWindow(hwnd);
                return 0;
            }
            if (id == IDC_VIEWER_BTN_FILES) {
                ShowFileManagerWindow(hwnd);
                return 0;
            }
            if (id == IDC_VIEWER_BTN_CAD) {
                SendSpecialKeyToActiveClient(1);
                return 0;
            }
            if (id == IDC_VIEWER_BTN_LOCK) {
                SendSpecialKeyToActiveClient(2);
                return 0;
            }
            if (id == IDC_VIEWER_BTN_TASKMGR) {
                SendSpecialKeyToActiveClient(3);
                return 0;
            }
            break;
        }

        case WM_APP_RESIZE: {
            int imgW = (int)wParam;
            int imgH = (int)lParam;

            // Calculate window size to fit image
            RECT rc = {0, 0, imgW, imgH};
            DWORD style = GetWindowLong(hwnd, GWL_STYLE);
            AdjustWindowRect(&rc, style, FALSE);

            RECT curRect;
            GetWindowRect(hwnd, &curRect);

            // Limit to screen size
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            int newW = (int)std::min((LONG)(rc.right - rc.left), (LONG)(screenW - 100));
            int newH = (int)std::min((LONG)(rc.bottom - rc.top), (LONG)(screenH - 100));

            MoveWindow(hwnd, curRect.left, curRect.top, newW, newH, TRUE);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            if (!self || !self->dropHandler) {
                DragFinish(drop);
                return 0;
            }

            UINT fileCount = DragQueryFileA(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> droppedFiles;
            droppedFiles.reserve(fileCount);

            for (UINT i = 0; i < fileCount; i++) {
                UINT pathLen = DragQueryFileA(drop, i, nullptr, 0);
                std::vector<char> buffer(pathLen + 1);
                DragQueryFileA(drop, i, buffer.data(), (UINT)buffer.size());
                droppedFiles.push_back(buffer.data());
            }

            DragFinish(drop);
            self->dropHandler(droppedFiles);
            return 0;
        }

        case WM_APP_DISCONNECT:
            // Close window immediately when agent disconnects
            DestroyWindow(hwnd);
            return 0;

        case WM_APP + 10: {
            std::string* pMsg = (std::string*)lParam;
            if (pMsg) {
                if (!g_hViewerChatWnd) {
                    ShowViewerChatWindow(hwnd);
                }
                if (g_hViewerChatList) {
                    std::string entry = "[Agent]: " + *pMsg;
                    SendMessageA(g_hViewerChatList, LB_ADDSTRING, 0, (LPARAM)entry.c_str());
                    SendMessage(g_hViewerChatList, WM_VSCROLL, SB_BOTTOM, 0);
                }
                delete pMsg;
            }
            return 0;
        }

        case WM_APP + 11: {
            std::vector<uint8_t>* pData = (std::vector<uint8_t>*)lParam;
            if (pData && pData->size() >= sizeof(FileListResponseHeader)) {
                FileListResponseHeader* hdr = (FileListResponseHeader*)pData->data();
                uint32_t count = hdr->itemCount;
                FileItem* items = (FileItem*)(pData->data() + sizeof(FileListResponseHeader));
                UpdateRemoteFileListInUI(hdr->path, items, count);
                delete pData;
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);
            int winW = rc.right - rc.left;
            int winH = rc.bottom - rc.top;

            // Double buffering
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, winW, winH);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // Dark background
            HBRUSH bgBrush = CreateSolidBrush(RGB(20, 20, 25));
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            if (self) {
                EnterCriticalSection(&self->cs);
                if (self->hasFrame && !self->framePixels.empty()) {
                    double scale = self->ComputeScale(winW, winH);
                    int destW = (int)(self->imgWidth * scale);
                    int destH = (int)(self->imgHeight * scale);
                    int destX = (winW - destW) / 2;
                    int destY = (winH - destH) / 2;

                    SetStretchBltMode(memDC, HALFTONE);
                    StretchDIBits(memDC,
                                  destX, destY, destW, destH,
                                  0, 0, self->imgWidth, self->imgHeight,
                                  self->framePixels.data(),
                                  &self->bmpInfo,
                                  DIB_RGB_COLORS, SRCCOPY);
                } else {
                    // Show "Connecting..." text
                    SetTextColor(memDC, RGB(100, 100, 120));
                    SetBkMode(memDC, TRANSPARENT);
                    RECT textRc = rc;
                    DrawTextA(memDC, "Waiting for frames...", -1, &textRc,
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                LeaveCriticalSection(&self->cs);
            }

            BitBlt(hdc, 0, 0, winW, winH, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        // Mouse events
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            if (!self || !self->hasFrame) break;

            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            if (!self->IsInsideImage(x, y)) return 0;

            int imgX, imgY;
            self->ConvertWindowToImageCoords(x, y, imgX, imgY);

            uint8_t mouseType = 1;
            if (msg == WM_LBUTTONDOWN) mouseType = 2;
            else if (msg == WM_LBUTTONUP) mouseType = 3;
            else if (msg == WM_RBUTTONDOWN) mouseType = 4;
            else if (msg == WM_RBUTTONUP) mouseType = 5;

            self->SendMouseEvent(mouseType, imgX, imgY);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!self || !self->hasFrame) break;

            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd, &pt);

            if (!self->IsInsideImage(pt.x, pt.y)) return 0;

            int imgX, imgY;
            self->ConvertWindowToImageCoords(pt.x, pt.y, imgX, imgY);

            int16_t delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            self->SendMouseEvent(6, imgX, imgY, delta);
            return 0;
        }

        // Keyboard events
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            uint16_t vk = (uint16_t)wParam;

            // Local hotkeys
            if (vk == VK_F11) {
                if (self) self->ToggleFullscreen();
                return 0;
            }
            if (vk == VK_ESCAPE && self && self->isFullscreen) {
                self->ToggleFullscreen();
                return 0;
            }
            // Zoom controls: Ctrl+Plus, Ctrl+Minus, Ctrl+0
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (vk == VK_OEM_PLUS || vk == VK_ADD) {
                    g_viewerConfig.customScale += 0.1f;
                    g_viewerConfig.zoomMode = 2;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) {
                    g_viewerConfig.customScale -= 0.1f;
                    if (g_viewerConfig.customScale < 0.1f) g_viewerConfig.customScale = 0.1f;
                    g_viewerConfig.zoomMode = 2;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (vk == '0') {
                    g_viewerConfig.zoomMode = 0;  // Reset to fit
                    g_viewerConfig.customScale = 1.0f;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            if (self) self->SendKeyEvent(vk, true);
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            uint16_t vk = (uint16_t)wParam;
            if (vk == VK_F11 || vk == VK_ESCAPE) return 0;
            if (self) self->SendKeyEvent(vk, false);
            return 0;
        }

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    bool Create(const std::string& title) {
        HINSTANCE hInstance = GetModuleHandle(nullptr);

        WNDCLASS wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = TEXT("RCViewerClass");
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;

        if (!RegisterClass(&wc)) {
            // May already be registered
        }

        hWnd = CreateWindow(
            wc.lpszClassName,
            title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            windowWidth, windowHeight,
            nullptr, nullptr, hInstance, this
        );

        if (!hWnd) return false;

        DragAcceptFiles(hWnd, TRUE);
        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
        return true;
    }

    void MessageLoop() {
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
};

// ============================================================================
// VIEWER CLIENT
// ============================================================================

class ViewerClient {
private:
    SOCKET sock = INVALID_SOCKET;
    SecureChannel crypto;
    ViewerWindow window;
    HANDLE recvThread = nullptr;
    bool running = false;
    bool reconnectRecommended = false;
    uint32_t sequence = 0;
    uint32_t nextTransferId = 1;
    ContactManager contacts;
    CRITICAL_SECTION cs;

public:
    ViewerClient() {
        InitializeCriticalSection(&cs);
    }

    ~ViewerClient() {
        Disconnect();
        DeleteCriticalSection(&cs);
    }

    bool Connect(const std::string& ip, uint16_t port) {
        if (!NetworkHelper::InitWinsock()) {
            Console::PrintError("Failed to initialize Winsock");
            return false;
        }

        sock = NetworkHelper::CreateTCPSocket();
        if (sock == INVALID_SOCKET) {
            Console::PrintError("Failed to create socket");
            return false;
        }

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(port);

        if (getaddrinfo(ip.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
            Console::PrintError("Failed to resolve IP or hostname: " + ip);
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }

        if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
            Console::PrintError("Connection failed: " + std::to_string(WSAGetLastError()));
            freeaddrinfo(res);
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }
        freeaddrinfo(res);

        NetworkHelper::SetSocketNoDelay(sock);
        NetworkHelper::SetSocketKeepalive(sock);
        NetworkHelper::SetSocketBuffers(sock);
        NetworkHelper::SetSocketTimeout(sock, 15000, 15000);

        Console::PrintSuccess("Connected to agent!");
        return true;
    }

    bool PerformHandshake() {
        Console::PrintInfo("Starting secure handshake...");

        if (!crypto.Initialize()) {
            Console::PrintError("Failed to initialize encryption");
            return false;
        }

        // Send handshake init
        HandshakeInit init{};
        std::snprintf(init.version, sizeof(init.version), "%s", RC_VERSION);
        std::snprintf(init.deviceId, sizeof(init.deviceId), "%s", DeviceID::Generate().c_str());
        init.unattendedMode = 0;

        if (!NetworkHelper::SendPacket(sock, MSG_HANDSHAKE_INIT, &init, sizeof(init))) {
            Console::PrintError("Failed to send handshake init");
            return false;
        }

        // Receive agent's public key
        PacketHeader header;
        std::vector<uint8_t> payload;

        if (!NetworkHelper::RecvPacket(sock, header, payload, nullptr)) {
            Console::PrintError("Failed to receive agent public key");
            return false;
        }

        if (header.type != MSG_HANDSHAKE_PUBKEY || payload.size() < sizeof(ECDHKeyExchange)) {
            Console::PrintError("Invalid handshake response");
            return false;
        }

        ECDHKeyExchange* agentKey = (ECDHKeyExchange*)payload.data();

        // Send our public key
        const auto& pubKey = crypto.GetPublicKey();
        ECDHKeyExchange keyEx;
        memset(&keyEx, 0, sizeof(keyEx));
        memcpy(keyEx.publicKey, pubKey.data(), std::min(pubKey.size(), sizeof(keyEx.publicKey)));
        keyEx.keyLength = (uint32_t)pubKey.size();

        if (!NetworkHelper::SendPacket(sock, MSG_HANDSHAKE_PUBKEY, &keyEx, sizeof(keyEx))) {
            Console::PrintError("Failed to send public key");
            return false;
        }

        // Derive shared secret
        if (!crypto.DeriveSharedSecret(agentKey->publicKey, agentKey->keyLength)) {
            Console::PrintError("Failed to derive shared secret");
            return false;
        }

        // Wait for handshake OK
        if (!NetworkHelper::RecvPacket(sock, header, payload, nullptr)) {
            Console::PrintError("Failed to receive handshake confirmation");
            return false;
        }

        if (header.type != MSG_HANDSHAKE_OK) {
            Console::PrintError("Handshake failed");
            return false;
        }

        Console::PrintSuccess("Secure handshake completed!");
        return true;
    }

    bool RequestConnection() {
        Console::PrintInfo("Requesting connection...");
        Console::PrintInfo("Requesting FPS: " + std::to_string(g_viewerConfig.requestedFPS) + 
                          ", Quality: " + std::to_string(g_viewerConfig.jpegQuality));

        ConnectRequest req{};
        std::snprintf(req.viewerName, sizeof(req.viewerName), "%s", g_viewerConfig.viewerName.c_str());
        std::snprintf(req.password, sizeof(req.password), "%s", g_viewerConfig.password.c_str());
        req.requestedFPS = g_viewerConfig.requestedFPS;
        req.quality = (uint8_t)g_viewerConfig.jpegQuality;

        if (!NetworkHelper::SendPacket(sock, MSG_CONNECT_REQUEST, &req, sizeof(req), &crypto)) {
            Console::PrintError("Failed to send connection request");
            return false;
        }

        // Wait for response
        PacketHeader header;
        std::vector<uint8_t> payload;

        if (!NetworkHelper::RecvPacket(sock, header, payload, &crypto)) {
            Console::PrintError("No response from agent");
            return false;
        }

        if (header.type == MSG_CONNECT_ACCEPT) {
            Console::PrintSuccess("Connection accepted by agent!");
            return true;
        } else if (header.type == MSG_CONNECT_REJECT) {
            Console::PrintWarning("Connection rejected by agent");
            return false;
        }

        Console::PrintError("Unexpected response");
        return false;
    }

    bool SendFile(const std::string& filePath) {
        if (sock == INVALID_SOCKET || !crypto.IsReady()) {
            Console::PrintError("Cannot send file: session is not ready");
            return false;
        }

        std::ifstream file(filePath.c_str(), std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            Console::PrintError("Failed to open file: " + filePath);
            return false;
        }

        std::ifstream::pos_type sizePos = file.tellg();
        if (sizePos < 0) {
            Console::PrintError("Failed to read file size: " + filePath);
            return false;
        }

        uint64_t fileSize = (uint64_t)sizePos;
        file.seekg(0, std::ios::beg);

        std::string fileName = SanitizeFileName(GetBaseFileName(filePath));
        if (fileName.empty()) fileName = "file.bin";

        const uint32_t chunkSize = 32 * 1024;
        uint32_t transferId = nextTransferId++;

        Console::PrintInfo("Uploading " + fileName + " (" + std::to_string((unsigned long long)fileSize) + " bytes)");

        FileTransferBegin begin{};
        begin.transferId = transferId;
        begin.fileSize = fileSize;
        begin.chunkSize = chunkSize;
        std::snprintf(begin.fileName, sizeof(begin.fileName), "%s", fileName.c_str());

        if (!NetworkHelper::SendPacket(sock, MSG_FILE_TRANSFER_BEGIN, &begin, sizeof(begin), &crypto, sequence++)) {
            Console::PrintError("Failed to send file transfer header");
            return false;
        }

        std::vector<uint8_t> buffer(chunkSize);
        uint64_t sentBytes = 0;
        uint32_t chunkIndex = 0;

        while (file) {
            file.read((char*)buffer.data(), buffer.size());
            std::streamsize chunkBytes = file.gcount();
            if (chunkBytes <= 0) {
                break;
            }

            std::vector<uint8_t> payload(sizeof(FileTransferChunkHeader) + (size_t)chunkBytes);
            FileTransferChunkHeader chunkHeader{};
            chunkHeader.transferId = transferId;
            chunkHeader.chunkIndex = chunkIndex++;
            chunkHeader.chunkSize = (uint32_t)chunkBytes;

            memcpy(payload.data(), &chunkHeader, sizeof(chunkHeader));
            memcpy(payload.data() + sizeof(chunkHeader), buffer.data(), (size_t)chunkBytes);

            if (!NetworkHelper::SendPacket(sock, MSG_FILE_TRANSFER_CHUNK, payload.data(), payload.size(), &crypto, sequence++)) {
                Console::PrintError("Failed while sending file chunk");
                return false;
            }

            sentBytes += (uint64_t)chunkBytes;
        }

        FileTransferEnd end{};
        end.transferId = transferId;
        end.status = 0;
        NetworkHelper::SendPacket(sock, MSG_FILE_TRANSFER_END, &end, sizeof(end), &crypto, sequence++);

        Console::PrintSuccess("File sent: " + fileName);
        return true;
    }

    void HandleDroppedFiles(const std::vector<std::string>& files) {
        if (files.empty()) return;

        if (sock == INVALID_SOCKET || !crypto.IsReady()) {
            Console::PrintWarning("Drop ignored because no active session is connected");
            return;
        }

        for (size_t i = 0; i < files.size(); i++) {
            SendFile(files[i]);
        }
    }

    static DWORD WINAPI RecvThreadProc(LPVOID param) {
        ViewerClient* self = (ViewerClient*)param;
        self->ReceiveLoop();
        return 0;
    }

    void ReceiveLoop() {
        Console::PrintInfo("Frame receiver started");
        int errorCount = 0;
        const int MAX_RECV_ERRORS = 5;
        reconnectRecommended = false;

        while (running) {
            PacketHeader header;
            std::vector<uint8_t> payload;

            if (!NetworkHelper::RecvPacket(sock, header, payload, &crypto)) {
                errorCount++;
                if (errorCount >= MAX_RECV_ERRORS) {
                    Console::PrintError("Connection lost after " + std::to_string(errorCount) + " failures");
                    reconnectRecommended = true;
                    break;
                }
                Sleep(100);  // Brief pause before retry
                continue;
            }
            errorCount = 0;  // Reset on success

            if (header.type == MSG_FRAME_DATA && payload.size() > sizeof(FrameHeader)) {
                FrameHeader* fh = (FrameHeader*)payload.data();
                uint8_t* jpegData = payload.data() + sizeof(FrameHeader);
                size_t jpegSize = payload.size() - sizeof(FrameHeader);

                window.UpdateFrame(jpegData, jpegSize, fh->width, fh->height, fh->scale);
            }
            else if (header.type == MSG_PONG) {
                // Ping response received
            }
            else if (header.type == MSG_CHAT_TEXT && !payload.empty()) {
                std::string* pChatMsg = new std::string((char*)payload.data(), payload.size());
                PostMessageA(window.GetHWnd(), WM_APP + 10, 0, (LPARAM)pChatMsg);
            }
            else if (header.type == MSG_FILE_LIST_RESP && payload.size() >= sizeof(FileListResponseHeader)) {
                std::vector<uint8_t>* pData = new std::vector<uint8_t>(payload);
                PostMessageA(window.GetHWnd(), WM_APP + 11, 0, (LPARAM)pData);
            }
            else if (header.type == MSG_DISCONNECT) {
                Console::PrintInfo("Agent disconnected gracefully");
                reconnectRecommended = false;
                break;
            }
        }

        running = false;
        PostMessage(window.GetHWnd(), WM_APP_DISCONNECT, 0, 0);
    }

    void Disconnect() {
        running = false;

        if (sock != INVALID_SOCKET) {
            NetworkHelper::SendPacket(sock, MSG_DISCONNECT, nullptr, 0, &crypto);
            closesocket(sock);
            sock = INVALID_SOCKET;
        }

        if (recvThread) {
            WaitForSingleObject(recvThread, 2000);
            CloseHandle(recvThread);
            recvThread = nullptr;
        }

        crypto.Cleanup();
        NetworkHelper::CleanupWinsock();
    }

    void SendSpecialKey(uint8_t keyCmd) {
        if (sock != INVALID_SOCKET) {
            SpecialKeyPacket pkt{};
            pkt.keyCmd = keyCmd;
            EnterCriticalSection(&cs);
            NetworkHelper::SendPacket(sock, MSG_SPECIAL_KEY, &pkt, sizeof(pkt), &crypto, sequence++);
            LeaveCriticalSection(&cs);
        }
    }

    void SendChatMessage(const std::string& text) {
        if (sock != INVALID_SOCKET) {
            EnterCriticalSection(&cs);
            NetworkHelper::SendPacket(sock, MSG_CHAT_TEXT, (const void*)text.data(), text.size(), &crypto, sequence++);
            LeaveCriticalSection(&cs);
        }
    }

    void RequestRemoteFileList(const std::string& path) {
        if (sock != INVALID_SOCKET) {
            FileListRequest req{};
            snprintf(req.path, sizeof(req.path), "%s", path.c_str());
            EnterCriticalSection(&cs);
            NetworkHelper::SendPacket(sock, MSG_FILE_LIST_REQ, &req, sizeof(req), &crypto, sequence++);
            LeaveCriticalSection(&cs);
        }
    }

    void RequestRemoteFileDelete(const std::string& path) {
        if (sock != INVALID_SOCKET) {
            FileDeleteRequest req{};
            snprintf(req.path, sizeof(req.path), "%s", path.c_str());
            EnterCriticalSection(&cs);
            NetworkHelper::SendPacket(sock, MSG_FILE_DELETE_REQ, &req, sizeof(req), &crypto, sequence++);
            LeaveCriticalSection(&cs);
        }
    }

    void RequestRemoteFileMkdir(const std::string& path) {
        if (sock != INVALID_SOCKET) {
            FileMkdirRequest req{};
            snprintf(req.path, sizeof(req.path), "%s", path.c_str());
            EnterCriticalSection(&cs);
            NetworkHelper::SendPacket(sock, MSG_FILE_MKDIR_REQ, &req, sizeof(req), &crypto, sequence++);
            LeaveCriticalSection(&cs);
        }
    }

    void Run() {
        g_pViewerClient = this;
        running = true;
        reconnectRecommended = false;

        // Create window
        std::string title = "Remote Viewer - " + g_viewerConfig.agentIP;
        if (!window.Create(title)) {
            Console::PrintError("Failed to create viewer window");
            g_pViewerClient = nullptr;
            return;
        }

        window.SetSocket(sock);
        window.SetCrypto(&crypto);
        window.SetDropHandler([this](const std::vector<std::string>& files) {
            this->HandleDroppedFiles(files);
        });

        // Start receive thread
        recvThread = CreateThread(nullptr, 0, RecvThreadProc, this, 0, nullptr);

        // Run message loop
        window.MessageLoop();

        Disconnect();
        g_pViewerClient = nullptr;
    }

    bool ShouldReconnect() const { return reconnectRecommended; }

    ContactManager& GetContactManager() { return contacts; }
};

void SendSpecialKeyToActiveClient(uint8_t keyCmd) {
    if (g_pViewerClient) {
        g_pViewerClient->SendSpecialKey(keyCmd);
    }
}

bool StartSessionWithReconnect(ViewerClient& client) {
    int attempts = std::max(1, g_viewerConfig.reconnectAttempts);

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            Console::PrintWarning("Reconnecting (attempt " + std::to_string(attempt) +
                                 " of " + std::to_string(attempts) + ")...");
            Sleep(g_viewerConfig.reconnectDelayMs);
        }

        if (!client.Connect(g_viewerConfig.agentIP, g_viewerConfig.port)) {
            client.Disconnect();
            continue;
        }

        if (!client.PerformHandshake()) {
            client.Disconnect();
            continue;
        }

        if (!client.RequestConnection()) {
            client.Disconnect();
            return false;
        }

        Console::PrintSuccess("Starting remote session...");
        Console::PrintInfo("Press F11 for fullscreen, Ctrl+0 to reset zoom");
        Sleep(800);
        client.Run();

        bool shouldReconnect = g_viewerConfig.autoReconnect && client.ShouldReconnect();
        client.Disconnect();

        if (!shouldReconnect) {
            return true;
        }
    }

    Console::PrintError("Reconnect attempts exhausted.");
    return false;
}

// ============================================================================
// CONTACT MANAGEMENT UI
// ============================================================================

void ManageContacts(ContactManager& contacts) {
    while (true) {
        Console::ClearScreen();
        Console::SetColor(Console::BRIGHT_CYAN);
        std::cout << "\n   ===================================================\n";
        std::cout << "                  CONTACT MANAGEMENT\n";
        std::cout << "   ===================================================\n\n";
        Console::ResetColor();

        contacts.DisplayContacts();

        std::vector<std::string> options = {
            "Add New Contact",
            "Remove Contact",
            "Back to Main Menu"
        };

        int choice = Console::GetMenuChoice(options);

        switch (choice) {
            case 1: {
                std::string name = Console::GetInput("Contact Name");
                std::string ip = Console::GetInput("IP Address");
                std::string portStr = Console::GetInput("Port (default: 5000)");

                Contact c;
                c.name = name;
                c.ip = ip;
                c.port = 5000;
                if (!portStr.empty()) {
                    try {
                        int parsedPort = std::stoi(portStr);
                        if (parsedPort >= 1 && parsedPort <= 65535) {
                            c.port = (uint16_t)parsedPort;
                        } else {
                            Console::PrintWarning("Invalid port range, using default 5000");
                        }
                    } catch (...) {
                        Console::PrintWarning("Invalid port value, using default 5000");
                    }
                }
                c.lastConnected = time(nullptr);
                c.unattendedEnabled = false;

                contacts.AddContact(c);
                Console::PrintSuccess("Contact added!");
                Sleep(1000);
                break;
            }
            case 2: {
                contacts.DisplayContacts();
                std::string idxStr = Console::GetInput("Contact number to remove");
                try {
                    int idx = std::stoi(idxStr);
                    Contact* c = contacts.GetContactByIndex(idx);
                    if (c) {
                        contacts.RemoveContact(c->name);
                        Console::PrintSuccess("Contact removed!");
                    } else {
                        Console::PrintError("Invalid contact number");
                    }
                } catch (...) {
                    Console::PrintError("Invalid input");
                }
                Sleep(1000);
                break;
            }
            case 3:
            default:
                return;
        }
    }
}

// ============================================================================
// CONNECTION MENU
// ============================================================================

bool SelectConnection(ViewerClient& client) {
    ContactManager& contacts = client.GetContactManager();

    Console::ClearScreen();
    Console::SetColor(Console::BRIGHT_CYAN);
    std::cout << "\n   ===================================================\n";
    std::cout << "                   CONNECT TO AGENT\n";
    std::cout << "   ===================================================\n\n";
    Console::ResetColor();

    contacts.DisplayContacts();

    std::vector<std::string> options = {
        "Connect to Saved Contact",
        "Enter IP Manually",
        "Back"
    };

    int choice = Console::GetMenuChoice(options);

    switch (choice) {
        case 1: {
            if (contacts.GetAllContacts().empty()) {
                Console::PrintWarning("No saved contacts. Please add one first.");
                Sleep(1500);
                return false;
            }
            contacts.DisplayContacts();
            std::string idxStr = Console::GetInput("Select contact number");
            try {
                int idx = std::stoi(idxStr);
                Contact* c = contacts.GetContactByIndex(idx);
                if (c) {
                    g_viewerConfig.agentIP = c->ip;
                    g_viewerConfig.port = c->port;
                    c->lastConnected = time(nullptr);
                    contacts.AddContact(*c);  // Update last connected
                    return true;
                }
            } catch (...) {}
            Console::PrintError("Invalid selection");
            Sleep(1000);
            return false;
        }
        case 2: {
            std::string ip = Console::GetInput("Agent IP Address");
            std::string portStr = Console::GetInput("Port (default: 5000)");
            
            g_viewerConfig.agentIP = ip;
            g_viewerConfig.port = 5000;
            if (!portStr.empty()) {
                try {
                    int parsedPort = std::stoi(portStr);
                    if (parsedPort >= 1 && parsedPort <= 65535) {
                        g_viewerConfig.port = (uint16_t)parsedPort;
                    } else {
                        Console::PrintWarning("Invalid port range, using default 5000");
                    }
                } catch (...) {
                    Console::PrintWarning("Invalid port value, using default 5000");
                }
            }

            // Quick FPS/Quality settings
            Console::PrintInfo("Current settings - FPS: " + std::to_string(g_viewerConfig.requestedFPS) + 
                             ", Quality: " + std::to_string(g_viewerConfig.jpegQuality));
            std::string fpsStr = Console::GetInput("FPS (1-60, Enter to keep " + std::to_string(g_viewerConfig.requestedFPS) + ")");
            if (!fpsStr.empty()) {
                try {
                    int fps = std::stoi(fpsStr);
                    if (fps >= 1 && fps <= 60) g_viewerConfig.requestedFPS = fps;
                } catch (...) {}
            }

            // Ask to save
            std::string save = Console::GetInput("Save this contact? (y/n)");
            if (save == "y" || save == "Y") {
                std::string name = Console::GetInput("Contact name");
                Contact c;
                c.name = name;
                c.ip = ip;
                c.port = g_viewerConfig.port;
                c.lastConnected = time(nullptr);
                contacts.AddContact(c);
                Console::PrintSuccess("Contact saved!");
            }
            return true;
        }
        default:
            return false;
    }
}

// ============================================================================
// SETTINGS MENU
// ============================================================================

void ConfigureSettings() {
    Console::ClearScreen();
    Console::SetColor(Console::BRIGHT_CYAN);
    std::cout << "\n   ===================================================\n";
    std::cout << "                      SETTINGS\n";
    std::cout << "   ===================================================\n\n";
    Console::ResetColor();

    // Viewer name
    std::string name = Console::GetInput("Your name (for identification)");
    if (!name.empty()) g_viewerConfig.viewerName = name;

    // FPS
    Console::PrintInfo("Current FPS: " + std::to_string(g_viewerConfig.requestedFPS));
    std::string fpsStr = Console::GetInput("Requested FPS 1-60 (default: 30)");
    if (!fpsStr.empty()) {
        try {
            int fps = std::stoi(fpsStr);
            if (fps >= 1 && fps <= 60) {
                g_viewerConfig.requestedFPS = fps;
            } else {
                Console::PrintWarning("FPS must be between 1 and 60");
            }
        } catch (...) {
            Console::PrintError("Invalid FPS value");
        }
    }

    // Quality
    Console::PrintInfo("Current Quality: " + std::to_string(g_viewerConfig.jpegQuality));
    std::string qualityStr = Console::GetInput("JPEG Quality 1-100 (default: 75)");
    if (!qualityStr.empty()) {
        try {
            int quality = std::stoi(qualityStr);
            if (quality >= 1 && quality <= 100) {
                g_viewerConfig.jpegQuality = quality;
            } else {
                Console::PrintWarning("Quality must be between 1 and 100");
            }
        } catch (...) {
            Console::PrintError("Invalid quality value");
        }
    }

    // Auto scale
    std::string autoScale = Console::GetInput("Auto-resize window to fit? (y/n, default: y)");
    g_viewerConfig.autoScale = (autoScale != "n" && autoScale != "N");

    // Auto reconnect
    std::string autoReconnect = Console::GetInput("Auto-reconnect on drop? (y/n, default: y)");
    g_viewerConfig.autoReconnect = (autoReconnect != "n" && autoReconnect != "N");

    if (g_viewerConfig.autoReconnect) {
        Console::PrintInfo("Current reconnect attempts: " + std::to_string(g_viewerConfig.reconnectAttempts));
        std::string retryStr = Console::GetInput("Reconnect attempts 1-10 (default: 3)");
        if (!retryStr.empty()) {
            try {
                int retries = std::stoi(retryStr);
                if (retries >= 1 && retries <= 10) {
                    g_viewerConfig.reconnectAttempts = retries;
                } else {
                    Console::PrintWarning("Reconnect attempts must be between 1 and 10");
                }
            } catch (...) {
                Console::PrintError("Invalid reconnect attempts value");
            }
        }
    }

    Console::PrintSuccess("Settings saved!");
    Sleep(1000);
}

// ============================================================================
// MAIN MENU
// ============================================================================

// ============================================================================
// GRAPHICAL GUI CONNECTION LAUNCHER
// ============================================================================

#define IDC_EDIT_IP      1001
#define IDC_EDIT_PORT    1002
#define IDC_BTN_CONNECT  1003
#define IDC_LIST_CONTACTS 1005
#define IDC_EDIT_PASS    1006

#define IDC_VIEWER_CHAT_LIST   3101
#define IDC_VIEWER_CHAT_INPUT  3102
#define IDC_VIEWER_CHAT_SEND   3103

static HWND g_hViewerChatInput = nullptr;

static LRESULT CALLBACK ViewerChatProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hViewerChatList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL, 15, 15, 410, 190, hwnd, (HMENU)IDC_VIEWER_CHAT_LIST, nullptr, nullptr);
        SendMessage(g_hViewerChatList, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hViewerChatInput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 15, 215, 310, 28, hwnd, (HMENU)IDC_VIEWER_CHAT_INPUT, nullptr, nullptr);
        SendMessage(g_hViewerChatInput, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hSendBtn = CreateWindowA("BUTTON", "Send",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 335, 215, 90, 28, hwnd, (HMENU)IDC_VIEWER_CHAT_SEND, nullptr, nullptr);
        SendMessage(hSendBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_VIEWER_CHAT_SEND) {
            char textBuf[512] = {0};
            GetWindowTextA(g_hViewerChatInput, textBuf, sizeof(textBuf));
            if (strlen(textBuf) > 0) {
                std::string msgStr = textBuf;
                SetWindowTextA(g_hViewerChatInput, "");
                std::string myEntry = "[Me]: " + msgStr;
                SendMessageA(g_hViewerChatList, LB_ADDSTRING, 0, (LPARAM)myEntry.c_str());
                SendMessage(g_hViewerChatList, WM_VSCROLL, SB_BOTTOM, 0);

                if (g_pViewerClient) {
                    g_pViewerClient->SendChatMessage(msgStr);
                }
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        g_hViewerChatWnd = nullptr;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ShowViewerChatWindow(HWND parentHwnd = nullptr) {
    if (g_hViewerChatWnd) {
        SetForegroundWindow(g_hViewerChatWnd);
        return;
    }
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = ViewerChatProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RCViewerChatClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    g_hViewerChatWnd = CreateWindowA(
        wc.lpszClassName,
        "Encrypted Live Chat - Remote Controller Viewer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 455, 295,
        parentHwnd, nullptr, hInstance, nullptr
    );

    if (g_hViewerChatWnd) {
        ShowWindow(g_hViewerChatWnd, SW_SHOW);
        UpdateWindow(g_hViewerChatWnd);
    }
}

// --- DUAL PANE REMOTE FILE MANAGER ---

#define IDC_FM_LOCAL_PATH   3201
#define IDC_FM_LOCAL_LIST   3202
#define IDC_FM_LOCAL_UP     3203
#define IDC_FM_REMOTE_PATH  3204
#define IDC_FM_REMOTE_LIST  3205
#define IDC_FM_REMOTE_UP    3206
#define IDC_FM_BTN_UPLOAD   3207
#define IDC_FM_BTN_DNLOAD   3208
#define IDC_FM_BTN_DELETE   3209
#define IDC_FM_BTN_MKDIR    3210

static HWND g_hFmWnd = nullptr;
static HWND g_hFmLocalPath = nullptr;
static HWND g_hFmLocalList = nullptr;
static HWND g_hFmRemotePath = nullptr;
static HWND g_hFmRemoteList = nullptr;

static std::string g_currentLocalPath = "C:\\";
static std::string g_currentRemotePath = "C:\\";

void PopulateLocalFileList(const std::string& path) {
    g_currentLocalPath = path;
    if (g_hFmLocalPath) SetWindowTextA(g_hFmLocalPath, g_currentLocalPath.c_str());
    if (!g_hFmLocalList) return;

    SendMessage(g_hFmLocalList, LB_RESETCONTENT, 0, 0);

    std::string searchPath = g_currentLocalPath;
    if (searchPath.empty()) searchPath = "C:\\";
    if (searchPath.back() != '\\' && searchPath.back() != '/') searchPath += "\\";
    searchPath += "*";

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            std::string prefix = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "[DIR] " : "      ";
            std::string entry = prefix + fd.cFileName;
            SendMessageA(g_hFmLocalList, LB_ADDSTRING, 0, (LPARAM)entry.c_str());
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
}

void UpdateRemoteFileListInUI(const char* path, const FileItem* items, uint32_t count) {
    g_currentRemotePath = path;
    if (g_hFmRemotePath) SetWindowTextA(g_hFmRemotePath, g_currentRemotePath.c_str());
    if (!g_hFmRemoteList) return;

    SendMessage(g_hFmRemoteList, LB_RESETCONTENT, 0, 0);

    for (uint32_t i = 0; i < count; i++) {
        std::string prefix = items[i].isDir ? "[DIR] " : "      ";
        std::string entry = prefix + items[i].name;
        SendMessageA(g_hFmRemoteList, LB_ADDSTRING, 0, (LPARAM)entry.c_str());
    }
}

static LRESULT CALLBACK FileManagerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLocalTitle = CreateWindowA("STATIC", "Local Computer", WS_CHILD | WS_VISIBLE, 15, 10, 310, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hLocalTitle, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hFmLocalPath = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "C:\\", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 15, 32, 260, 24, hwnd, (HMENU)IDC_FM_LOCAL_PATH, nullptr, nullptr);
        SendMessage(g_hFmLocalPath, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hLocalUp = CreateWindowA("BUTTON", "Up", WS_CHILD | WS_VISIBLE, 280, 32, 45, 24, hwnd, (HMENU)IDC_FM_LOCAL_UP, nullptr, nullptr);
        SendMessage(hLocalUp, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hFmLocalList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 15, 60, 310, 280, hwnd, (HMENU)IDC_FM_LOCAL_LIST, nullptr, nullptr);
        SendMessage(g_hFmLocalList, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hRemoteTitle = CreateWindowA("STATIC", "Remote Computer", WS_CHILD | WS_VISIBLE, 455, 10, 310, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hRemoteTitle, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hFmRemotePath = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "C:\\", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 455, 32, 260, 24, hwnd, (HMENU)IDC_FM_REMOTE_PATH, nullptr, nullptr);
        SendMessage(g_hFmRemotePath, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hRemoteUp = CreateWindowA("BUTTON", "Up", WS_CHILD | WS_VISIBLE, 720, 32, 45, 24, hwnd, (HMENU)IDC_FM_REMOTE_UP, nullptr, nullptr);
        SendMessage(hRemoteUp, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hFmRemoteList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 455, 60, 310, 280, hwnd, (HMENU)IDC_FM_REMOTE_LIST, nullptr, nullptr);
        SendMessage(g_hFmRemoteList, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hUpload = CreateWindowA("BUTTON", "Upload >>", WS_CHILD | WS_VISIBLE, 335, 100, 110, 32, hwnd, (HMENU)IDC_FM_BTN_UPLOAD, nullptr, nullptr);
        SendMessage(hUpload, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hDnload = CreateWindowA("BUTTON", "<< Download", WS_CHILD | WS_VISIBLE, 335, 140, 110, 32, hwnd, (HMENU)IDC_FM_BTN_DNLOAD, nullptr, nullptr);
        SendMessage(hDnload, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hDelete = CreateWindowA("BUTTON", "Delete Remote", WS_CHILD | WS_VISIBLE, 335, 190, 110, 32, hwnd, (HMENU)IDC_FM_BTN_DELETE, nullptr, nullptr);
        SendMessage(hDelete, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hMkdir = CreateWindowA("BUTTON", "New Folder", WS_CHILD | WS_VISIBLE, 335, 230, 110, 32, hwnd, (HMENU)IDC_FM_BTN_MKDIR, nullptr, nullptr);
        SendMessage(hMkdir, WM_SETFONT, (WPARAM)hFont, TRUE);

        PopulateLocalFileList(g_currentLocalPath);
        if (g_pViewerClient) {
            g_pViewerClient->RequestRemoteFileList(g_currentRemotePath);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_FM_LOCAL_UP) {
            size_t p = g_currentLocalPath.find_last_of("\\/");
            if (p != std::string::npos && p > 0) {
                std::string parentPath = g_currentLocalPath.substr(0, p);
                if (parentPath.length() == 2 && parentPath[1] == ':') parentPath += "\\";
                PopulateLocalFileList(parentPath);
            }
            return 0;
        }

        if (id == IDC_FM_LOCAL_LIST && code == LBN_DBLCLK) {
            int sel = (int)SendMessage(g_hFmLocalList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[256] = {0};
                SendMessageA(g_hFmLocalList, LB_GETTEXT, sel, (LPARAM)text);
                std::string nameStr(text);
                if (nameStr.rfind("[DIR] ", 0) == 0) {
                    std::string dirName = nameStr.substr(6);
                    std::string newPath = g_currentLocalPath;
                    if (newPath.back() != '\\' && newPath.back() != '/') newPath += "\\";
                    newPath += dirName;
                    PopulateLocalFileList(newPath);
                }
            }
            return 0;
        }

        if (id == IDC_FM_REMOTE_UP) {
            size_t p = g_currentRemotePath.find_last_of("\\/");
            if (p != std::string::npos && p > 0) {
                std::string parentPath = g_currentRemotePath.substr(0, p);
                if (parentPath.length() == 2 && parentPath[1] == ':') parentPath += "\\";
                if (g_pViewerClient) g_pViewerClient->RequestRemoteFileList(parentPath);
            }
            return 0;
        }

        if (id == IDC_FM_REMOTE_LIST && code == LBN_DBLCLK) {
            int sel = (int)SendMessage(g_hFmRemoteList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[256] = {0};
                SendMessageA(g_hFmRemoteList, LB_GETTEXT, sel, (LPARAM)text);
                std::string nameStr(text);
                if (nameStr.rfind("[DIR] ", 0) == 0) {
                    std::string dirName = nameStr.substr(6);
                    std::string newPath = g_currentRemotePath;
                    if (newPath.back() != '\\' && newPath.back() != '/') newPath += "\\";
                    newPath += dirName;
                    if (g_pViewerClient) g_pViewerClient->RequestRemoteFileList(newPath);
                }
            }
            return 0;
        }

        if (id == IDC_FM_BTN_UPLOAD) {
            int sel = (int)SendMessage(g_hFmLocalList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[256] = {0};
                SendMessageA(g_hFmLocalList, LB_GETTEXT, sel, (LPARAM)text);
                std::string nameStr(text);
                if (nameStr.rfind("[DIR] ", 0) != 0) {
                    std::string fileName = nameStr.substr(6);
                    std::string fullLocalPath = g_currentLocalPath;
                    if (fullLocalPath.back() != '\\' && fullLocalPath.back() != '/') fullLocalPath += "\\";
                    fullLocalPath += fileName;
                    if (g_pViewerClient) {
                        g_pViewerClient->SendFile(fullLocalPath);
                        g_pViewerClient->RequestRemoteFileList(g_currentRemotePath);
                    }
                }
            }
            return 0;
        }

        if (id == IDC_FM_BTN_DELETE) {
            int sel = (int)SendMessage(g_hFmRemoteList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[256] = {0};
                SendMessageA(g_hFmRemoteList, LB_GETTEXT, sel, (LPARAM)text);
                std::string nameStr(text);
                std::string fileName = (nameStr.rfind("[DIR] ", 0) == 0) ? nameStr.substr(6) : nameStr.substr(6);
                std::string fullRemotePath = g_currentRemotePath;
                if (fullRemotePath.back() != '\\' && fullRemotePath.back() != '/') fullRemotePath += "\\";
                fullRemotePath += fileName;
                if (g_pViewerClient) {
                    g_pViewerClient->RequestRemoteFileDelete(fullRemotePath);
                    Sleep(200);
                    g_pViewerClient->RequestRemoteFileList(g_currentRemotePath);
                }
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        g_hFmWnd = nullptr;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ShowFileManagerWindow(HWND parentHwnd = nullptr) {
    if (g_hFmWnd) {
        SetForegroundWindow(g_hFmWnd);
        return;
    }
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = FileManagerProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RCFileManagerClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    g_hFmWnd = CreateWindowA(
        wc.lpszClassName,
        "Remote File Manager - Dual Pane",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 795, 395,
        parentHwnd, nullptr, hInstance, nullptr
    );

    if (g_hFmWnd) {
        ShowWindow(g_hFmWnd, SW_SHOW);
        UpdateWindow(g_hFmWnd);
    }
}

static HWND g_hIpEdit = nullptr;
static HWND g_hPortEdit = nullptr;
static HWND g_hPassEdit = nullptr;
static HWND g_hContactsList = nullptr;
static bool g_dialogConnected = false;

static LRESULT CALLBACK GuiDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLabel = CreateWindowA("STATIC", "Remote Controller v2.0 - Connect to Remote PC",
            WS_CHILD | WS_VISIBLE, 20, 15, 440, 25, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hIpLabel = CreateWindowA("STATIC", "IP Address:",
            WS_CHILD | WS_VISIBLE, 20, 50, 90, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hIpLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hIpEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "127.0.0.1",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 110, 48, 220, 24, hwnd, (HMENU)IDC_EDIT_IP, nullptr, nullptr);
        SendMessage(g_hIpEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hPortLabel = CreateWindowA("STATIC", "Port:",
            WS_CHILD | WS_VISIBLE, 345, 50, 40, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hPortLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "5000",
            WS_CHILD | WS_VISIBLE | ES_NUMBER, 385, 48, 65, 24, hwnd, (HMENU)IDC_EDIT_PORT, nullptr, nullptr);
        SendMessage(g_hPortEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hPassLabel = CreateWindowA("STATIC", "Password (Unattended Access):",
            WS_CHILD | WS_VISIBLE, 20, 85, 210, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hPassLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hPassEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_PASSWORD, 235, 83, 215, 24, hwnd, (HMENU)IDC_EDIT_PASS, nullptr, nullptr);
        SendMessage(g_hPassEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnConnect = CreateWindowA("BUTTON", "Connect Now",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 120, 430, 35, hwnd, (HMENU)IDC_BTN_CONNECT, nullptr, nullptr);
        SendMessage(hBtnConnect, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hContactLabel = CreateWindowA("STATIC", "Saved Contacts (Double-click to Connect):",
            WS_CHILD | WS_VISIBLE, 20, 165, 300, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hContactLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hContactsList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 20, 190, 430, 110, hwnd, (HMENU)IDC_LIST_CONTACTS, nullptr, nullptr);
        SendMessage(g_hContactsList, WM_SETFONT, (WPARAM)hFont, TRUE);

        ContactManager cm;
        std::vector<Contact> contacts = cm.GetAllContacts();
        if (contacts.empty()) {
            SendMessageA(g_hContactsList, LB_ADDSTRING, 0, (LPARAM)"No saved contacts. Enter IP above and click Connect.");
        } else {
            for (const auto& c : contacts) {
                std::string entry = c.name + " (" + c.ip + ":" + std::to_string(c.port) + ")";
                SendMessageA(g_hContactsList, LB_ADDSTRING, 0, (LPARAM)entry.c_str());
            }
        }

        std::string devId = "Device ID: " + DeviceID::Generate() + "  |  AES-256 Encrypted";
        HWND hFooter = CreateWindowA("STATIC", devId.c_str(),
            WS_CHILD | WS_VISIBLE, 20, 310, 430, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hFooter, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_BTN_CONNECT) {
            char ipBuf[256] = {0};
            char portBuf[32] = {0};
            char passBuf[64] = {0};
            GetWindowTextA(g_hIpEdit, ipBuf, sizeof(ipBuf));
            GetWindowTextA(g_hPortEdit, portBuf, sizeof(portBuf));
            GetWindowTextA(g_hPassEdit, passBuf, sizeof(passBuf));

            if (strlen(ipBuf) > 0) {
                g_viewerConfig.agentIP = ipBuf;
                g_viewerConfig.password = passBuf;
                try { g_viewerConfig.port = (uint16_t)std::stoi(portBuf); } catch (...) { g_viewerConfig.port = 5000; }
                g_dialogConnected = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }

        if (id == IDC_LIST_CONTACTS && code == LBN_DBLCLK) {
            int sel = (int)SendMessage(g_hContactsList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[256] = {0};
                SendMessageA(g_hContactsList, LB_GETTEXT, sel, (LPARAM)text);
                std::string str(text);
                size_t p1 = str.find('(');
                size_t p2 = str.find(':', p1);
                size_t p3 = str.find(')', p2);
                if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos) {
                    std::string ip = str.substr(p1 + 1, p2 - (p1 + 1));
                    std::string port = str.substr(p2 + 1, p3 - (p2 + 1));
                    SetWindowTextA(g_hIpEdit, ip.c_str());
                    SetWindowTextA(g_hPortEdit, port.c_str());
                    g_viewerConfig.agentIP = ip;
                    try { g_viewerConfig.port = (uint16_t)std::stoi(port); } catch (...) { g_viewerConfig.port = 5000; }
                    g_dialogConnected = true;
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

bool ShowGuiConnectionDialog() {
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = GuiDialogProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RCViewerLauncherClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 485;
    int winH = 380;
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    HWND hwnd = CreateWindowA(
        wc.lpszClassName,
        "Remote Controller v2.0 - Connect",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return false;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return g_dialogConnected;
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    EnableDPIAwareness();

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) {
        char ipBuf[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, argvW[1], -1, ipBuf, sizeof(ipBuf), nullptr, nullptr);
        g_viewerConfig.agentIP = ipBuf;
        if (argc >= 3) {
            char portBuf[32] = {0};
            WideCharToMultiByte(CP_UTF8, 0, argvW[2], -1, portBuf, sizeof(portBuf), nullptr, nullptr);
            try { g_viewerConfig.port = (uint16_t)std::stoi(portBuf); } catch (...) {}
        }
        if (argvW) LocalFree(argvW);

        ViewerClient client;
        StartSessionWithReconnect(client);
        client.Disconnect();
        return 0;
    }
    if (argvW) LocalFree(argvW);

    while (ShowGuiConnectionDialog()) {
        ViewerClient client;
        StartSessionWithReconnect(client);
        client.Disconnect();
        g_dialogConnected = false;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOW);
}

