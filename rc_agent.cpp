/*
 * Remote Controller - Agent Application
 * Captures screen, encodes to JPEG, sends encrypted frames
 * Receives and executes control commands
 */

#include "rc_common.h"
#include <objbase.h>
#include <shellapi.h>

// stb_image_write for JPEG encoding
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

struct AgentConfig {
    uint16_t port = RC_DEFAULT_PORT;
    float captureScale = 0.5f;
    int jpegQuality = 75;
    int targetFPS = 40;
    bool unattendedAccess = false;
    std::string unattendedPassword;
    std::string deviceId;
    std::string allowedViewerIP;  // Empty = accept any
};

AgentConfig g_config;
static HWND g_hAgentWnd = nullptr;
static HWND g_hAgentStatusLabel = nullptr;
static HWND g_hAgentChatWnd = nullptr;
static HWND g_hAgentChatList = nullptr;
void ShowAgentChatWindow();

// ============================================================================
// SCREEN CAPTURE
// ============================================================================

class ScreenCapture {
private:
    int fullWidth = 0;
    int fullHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    float scale = 0.5f;
    std::vector<uint8_t> pixelBuffer;
    std::vector<uint8_t> prevPixelBuffer;
    std::vector<uint8_t> rgbBuffer;

    // Get virtual screen bounds (handles multi-monitor and DPI)
    void GetVirtualScreenBounds(int& x, int& y, int& w, int& h) {
        x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        
        // Fallback to primary monitor if virtual screen fails
        if (w <= 0 || h <= 0) {
            x = 0;
            y = 0;
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
        }
    }

public:
    bool Initialize(float captureScale) {
        scale = captureScale;
        
        // Get the primary monitor size (more reliable)
        int vx, vy;
        GetVirtualScreenBounds(vx, vy, fullWidth, fullHeight);
        
        // For simplicity, use primary monitor only
        fullWidth = GetSystemMetrics(SM_CXSCREEN);
        fullHeight = GetSystemMetrics(SM_CYSCREEN);
        
        targetWidth = (int)(fullWidth * scale);
        targetHeight = (int)(fullHeight * scale);

        if (targetWidth < 1) targetWidth = 1;
        if (targetHeight < 1) targetHeight = 1;

        pixelBuffer.resize(targetWidth * targetHeight * 4);
        prevPixelBuffer.clear();
        rgbBuffer.resize(targetWidth * targetHeight * 3);

        Console::PrintSuccess("Screen capture initialized: " + 
                             std::to_string(fullWidth) + "x" + std::to_string(fullHeight) +
                             " -> " + std::to_string(targetWidth) + "x" + std::to_string(targetHeight));
        return true;
    }

    void Cleanup() {
        // Nothing to cleanup - we create/destroy DC each frame for reliability
    }

    ~ScreenCapture() {
        Cleanup();
    }

    // JPEG write callback - writes to vector
    static void JpegWriteCallback(void* context, void* data, int size) {
        std::vector<uint8_t>* buffer = (std::vector<uint8_t>*)context;
        uint8_t* bytes = (uint8_t*)data;
        buffer->insert(buffer->end(), bytes, bytes + size);
    }

    // Combined capture and encode with low-spec frame difference optimization
    bool CaptureAndEncode(std::vector<uint8_t>& jpegBuffer, int quality, bool* outChanged = nullptr) {
        if (outChanged) *outChanged = true;

        // Attach thread to active input desktop (preserves live feed when PC is locked or on Winlogon)
        HDESK hInputDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
        if (!hInputDesk) {
            hInputDesk = OpenDesktopA((LPSTR)"Winlogon", 0, FALSE, MAXIMUM_ALLOWED);
        }
        if (hInputDesk) {
            SetThreadDesktop(hInputDesk);
            CloseDesktop(hInputDesk);
        }

        // Create fresh DC each frame - with DISPLAY driver fallback for Lock Screen
        HDC hdcScreen = GetDC(nullptr);
        bool createdDC = false;
        if (!hdcScreen) {
            hdcScreen = CreateDCA("DISPLAY", nullptr, nullptr, nullptr);
            createdDC = true;
        }
        if (!hdcScreen) return false;

        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        if (!hdcMem) {
            if (createdDC) DeleteDC(hdcScreen); else ReleaseDC(nullptr, hdcScreen);
            return false;
        }

        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, targetWidth, targetHeight);
        if (!hBitmap) {
            DeleteDC(hdcMem);
            if (createdDC) DeleteDC(hdcScreen); else ReleaseDC(nullptr, hdcScreen);
            return false;
        }

        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
        SetStretchBltMode(hdcMem, COLORONCOLOR);
        SetBrushOrgEx(hdcMem, 0, 0, nullptr);

        // Capture from primary screen with CAPTUREBLT for Winlogon/Lock Screen & UAC support
        BOOL captureResult = StretchBlt(hdcMem, 0, 0, targetWidth, targetHeight,
                                        hdcScreen, 0, 0, fullWidth, fullHeight, SRCCOPY | CAPTUREBLT);
        if (!captureResult) {
            captureResult = StretchBlt(hdcMem, 0, 0, targetWidth, targetHeight,
                                       hdcScreen, 0, 0, fullWidth, fullHeight, SRCCOPY);
        }

        bool success = false;
        if (captureResult) {
            // Get bitmap pixels
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = targetWidth;
            bi.bmiHeader.biHeight = -targetHeight;  // Top-down (negative = top-down)
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            if (GetDIBits(hdcMem, hBitmap, 0, targetHeight, pixelBuffer.data(), &bi, DIB_RGB_COLORS)) {
                // Check if screen actually changed
                bool changed = true;
                if (!prevPixelBuffer.empty() && prevPixelBuffer.size() == pixelBuffer.size()) {
                    changed = FastFrameDiff::HasScreenChanged(prevPixelBuffer.data(), pixelBuffer.data(), pixelBuffer.size(), 50);
                }
                if (outChanged) *outChanged = changed;

                if (!changed && !prevPixelBuffer.empty()) {
                    // Screen unchanged! Skip JPEG compression & transmission to save CPU
                    SelectObject(hdcMem, hOldBitmap);
                    DeleteObject(hBitmap);
                    DeleteDC(hdcMem);
                    if (createdDC) DeleteDC(hdcScreen); else ReleaseDC(nullptr, hdcScreen);
                    return true;
                }

                prevPixelBuffer = pixelBuffer;

                // Convert BGRA to RGB for JPEG encoding
                for (int i = 0; i < targetWidth * targetHeight; i++) {
                    rgbBuffer[i * 3 + 0] = pixelBuffer[i * 4 + 2];  // R
                    rgbBuffer[i * 3 + 1] = pixelBuffer[i * 4 + 1];  // G
                    rgbBuffer[i * 3 + 2] = pixelBuffer[i * 4 + 0];  // B
                }

                jpegBuffer.clear();
                int encodeResult = stbi_write_jpg_to_func(JpegWriteCallback, &jpegBuffer,
                                                          targetWidth, targetHeight, 3,
                                                          rgbBuffer.data(), quality);
                success = (encodeResult != 0);
            }
        }

        // Cleanup
        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        if (createdDC) DeleteDC(hdcScreen); else ReleaseDC(nullptr, hdcScreen);

        return success;
    }

    // Legacy function for compatibility
    bool EncodeToJPEG(std::vector<uint8_t>& jpegBuffer, int quality) {
        return CaptureAndEncode(jpegBuffer, quality);
    }

    int GetWidth() const { return targetWidth; }
    int GetHeight() const { return targetHeight; }
    int GetFullWidth() const { return fullWidth; }
    int GetFullHeight() const { return fullHeight; }
    float GetScale() const { return scale; }
};

// ============================================================================
// INPUT SIMULATION
// ============================================================================

class InputSimulator {
private:
    bool keyStates[256] = {false};
    bool leftMouseDown = false;
    bool rightMouseDown = false;
    float invScale = 2.0f;
    int screenW = 0;
    int screenH = 0;

public:
    void Initialize(float captureScale) {
        invScale = 1.0f / captureScale;
        screenW = GetSystemMetrics(SM_CXSCREEN);
        screenH = GetSystemMetrics(SM_CYSCREEN);
        memset(keyStates, 0, sizeof(keyStates));
    }

    void HandleMouse(const ControlPacket& pkt) {
        HDESK hInputDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
        if (hInputDesk) {
            SetThreadDesktop(hInputDesk);
            CloseDesktop(hInputDesk);
        }

        // Convert from scaled image coords to full screen coords
        int fullX = (int)(pkt.x * invScale);
        int fullY = (int)(pkt.y * invScale);

        // Clamp to screen bounds
        if (fullX < 0) fullX = 0;
        if (fullY < 0) fullY = 0;
        if (fullX >= screenW) fullX = screenW - 1;
        if (fullY >= screenH) fullY = screenH - 1;

        // Move cursor
        SetCursorPos(fullX, fullY);

        if (pkt.mouseType == 1) return;  // Just move

        INPUT input{};
        input.type = INPUT_MOUSE;

        switch (pkt.mouseType) {
        case 2:  // Left down
            input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            leftMouseDown = true;
            break;
        case 3:  // Left up
            input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            leftMouseDown = false;
            break;
        case 4:  // Right down
            input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            rightMouseDown = true;
            break;
        case 5:  // Right up
            input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            rightMouseDown = false;
            break;
        case 6:  // Scroll
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = pkt.scrollDelta * WHEEL_DELTA;
            break;
        default:
            return;
        }

        SendInput(1, &input, sizeof(INPUT));
    }

    void HandleKeyboard(const ControlPacket& pkt) {
        HDESK hInputDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
        if (hInputDesk) {
            SetThreadDesktop(hInputDesk);
            CloseDesktop(hInputDesk);
        }

        uint16_t vk = pkt.vk;
        if (vk >= 256) vk &= 0xFF;

        bool down = pkt.keyDown != 0;

        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;

        SendInput(1, &input, sizeof(INPUT));
        keyStates[vk] = down;
    }

    void ReleaseAllInputs() {
        Console::PrintInfo("Releasing all held inputs...");

        // Release all keys
        for (int vk = 0; vk < 256; vk++) {
            if (keyStates[vk]) {
                INPUT input{};
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = (WORD)vk;
                input.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(INPUT));
                keyStates[vk] = false;
            }
        }

        // Release mouse buttons
        if (leftMouseDown) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &input, sizeof(INPUT));
            leftMouseDown = false;
        }
        if (rightMouseDown) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(1, &input, sizeof(INPUT));
            rightMouseDown = false;
        }
    }
};

// ============================================================================
// AGENT SERVER
// ============================================================================

class AgentServer {
private:
    struct ActiveFileTransfer {
        std::string fileName;
        std::string filePath;
        uint64_t expectedSize = 0;
        uint64_t receivedSize = 0;
        std::ofstream stream;
    };

    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET clientSocket = INVALID_SOCKET;
    SecureChannel crypto;
    ScreenCapture capture;
    InputSimulator input;
    bool running = false;
    bool connected = false;
    uint32_t frameId = 0;
    uint32_t sequence = 0;
    HANDLE captureThread = nullptr;
    HANDLE controlThread = nullptr;
    CRITICAL_SECTION cs;
    std::string connectedViewerIP;
    std::string connectedViewerName;
    std::map<uint32_t, ActiveFileTransfer> fileTransfers;

    std::string GetReceiveFolder() {
        CreateDirectoryA("received_files", nullptr);
        return "received_files";
    }

    std::string BuildUniqueFilePath(const std::string& baseName, uint32_t transferId) {
        std::string folder = GetReceiveFolder();
        std::string safeName = SanitizeFileName(baseName);
        if (safeName.empty()) safeName = "file.bin";

        std::string path = folder + "\\" + safeName;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            size_t dot = safeName.find_last_of('.');
            std::string stem = (dot == std::string::npos) ? safeName : safeName.substr(0, dot);
            std::string ext = (dot == std::string::npos) ? "" : safeName.substr(dot);
            path = folder + "\\" + stem + "_" + std::to_string(transferId) + ext;
        }
        return path;
    }

    void CleanupFileTransfers() {
        for (std::map<uint32_t, ActiveFileTransfer>::iterator it = fileTransfers.begin(); it != fileTransfers.end(); ++it) {
            if (it->second.stream.is_open()) {
                it->second.stream.close();
            }
        }
        fileTransfers.clear();
    }

    bool HandleFileTransferBegin(const FileTransferBegin& begin) {
        ActiveFileTransfer& transfer = fileTransfers[begin.transferId];

        if (transfer.stream.is_open()) {
            transfer.stream.close();
        }

        transfer.fileName = begin.fileName;
        transfer.expectedSize = begin.fileSize;
        transfer.receivedSize = 0;
        transfer.filePath = BuildUniqueFilePath(transfer.fileName, begin.transferId);
        transfer.stream.open(transfer.filePath.c_str(), std::ios::binary | std::ios::out);

        if (!transfer.stream.is_open()) {
            Console::PrintError("Failed to open file for writing: " + transfer.filePath);
            fileTransfers.erase(begin.transferId);
            return false;
        }

        Console::PrintInfo("Receiving file: " + transfer.fileName + " -> " + transfer.filePath);
        return true;
    }

    bool HandleFileTransferChunk(const FileTransferChunkHeader& chunkHeader, const uint8_t* chunkData, size_t chunkSize) {
        std::map<uint32_t, ActiveFileTransfer>::iterator it = fileTransfers.find(chunkHeader.transferId);
        if (it == fileTransfers.end()) {
            return false;
        }

        ActiveFileTransfer& transfer = it->second;
        if (!transfer.stream.is_open()) {
            return false;
        }

        transfer.stream.write((const char*)chunkData, (std::streamsize)chunkSize);
        if (!transfer.stream.good()) {
            Console::PrintError("Failed while writing file chunk: " + transfer.fileName);
            transfer.stream.close();
            fileTransfers.erase(it);
            return false;
        }

        transfer.receivedSize += (uint64_t)chunkSize;
        return true;
    }

    void HandleFileTransferEnd(const FileTransferEnd& end) {
        std::map<uint32_t, ActiveFileTransfer>::iterator it = fileTransfers.find(end.transferId);
        if (it == fileTransfers.end()) {
            return;
        }

        ActiveFileTransfer& transfer = it->second;
        if (transfer.stream.is_open()) {
            transfer.stream.close();
        }

        Console::PrintSuccess("File received: " + transfer.filePath +
                             " (" + std::to_string((unsigned long long)transfer.receivedSize) + " bytes)");
        fileTransfers.erase(it);
    }

public:
    AgentServer() {
        InitializeCriticalSection(&cs);
    }

    ~AgentServer() {
        Stop();
        DeleteCriticalSection(&cs);
    }

    bool Start(const AgentConfig& config) {
        g_config = config;

        if (!NetworkHelper::InitWinsock()) {
            Console::PrintError("Failed to initialize Winsock");
            return false;
        }

        listenSocket = NetworkHelper::CreateTCPSocket();
        if (listenSocket == INVALID_SOCKET) {
            Console::PrintError("Failed to create socket");
            return false;
        }

        // Allow address reuse
        int opt = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config.port);

        if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            Console::PrintError("Failed to bind to port " + std::to_string(config.port));
            closesocket(listenSocket);
            return false;
        }

        if (listen(listenSocket, 1) == SOCKET_ERROR) {
            Console::PrintError("Failed to listen");
            closesocket(listenSocket);
            return false;
        }

        if (!capture.Initialize(config.captureScale)) {
            Console::PrintError("Failed to initialize screen capture");
            closesocket(listenSocket);
            return false;
        }

        input.Initialize(config.captureScale);

        running = true;

        Console::PrintSuccess("Agent server started on port " + std::to_string(config.port));
        Console::PrintInfo("Device ID: " + config.deviceId);
        Console::PrintInfo("Local IP: " + NetworkHelper::GetLocalIP());
        Console::PrintInfo("Unattended Access: " + std::string(config.unattendedAccess ? "ENABLED" : "DISABLED"));

        return true;
    }

    void Stop() {
        running = false;
        connected = false;

        if (captureThread) {
            WaitForSingleObject(captureThread, 2000);
            CloseHandle(captureThread);
            captureThread = nullptr;
        }

        if (controlThread) {
            WaitForSingleObject(controlThread, 2000);
            CloseHandle(controlThread);
            controlThread = nullptr;
        }

        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }

        if (listenSocket != INVALID_SOCKET) {
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }

        input.ReleaseAllInputs();
        CleanupFileTransfers();
        capture.Cleanup();
        crypto.Cleanup();

        NetworkHelper::CleanupWinsock();
    }

    void DisconnectClient() {
        connected = false;
        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }
    }

    void SendChatMessage(const std::string& text) {
        if (clientSocket != INVALID_SOCKET && connected) {
            EnterCriticalSection(&cs);
            NetworkHelper::SendPacket(clientSocket, MSG_CHAT_TEXT, (const void*)text.data(), text.size(), &crypto, sequence++);
            LeaveCriticalSection(&cs);
        }
    }

    void TriggerWindowsSAS() {
        // 1. Enable Software SAS Generation policy in Windows Registry
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            DWORD dwValue = 3; // Allow Services and Applications to generate SAS
            RegSetValueExA(hKey, "SoftwareSASGeneration", 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));
            RegCloseKey(hKey);
        }

        // 2. Attach thread to active Winlogon/input desktop
        HDESK hInputDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
        if (!hInputDesk) {
            hInputDesk = OpenDesktopA((LPSTR)"Winlogon", 0, FALSE, MAXIMUM_ALLOWED);
        }
        if (hInputDesk) {
            SetThreadDesktop(hInputDesk);
            CloseDesktop(hInputDesk);
        }

        // 3. Invoke SendSAS from sas.dll (Official Windows SAS API)
        HMODULE hSas = LoadLibraryA("sas.dll");
        if (hSas) {
            typedef VOID (WINAPI *SendSASProc)(BOOL);
            SendSASProc pSendSAS = (SendSASProc)GetProcAddress(hSas, "SendSAS");
            if (pSendSAS) {
                pSendSAS(FALSE);
                FreeLibrary(hSas);
            } else {
                FreeLibrary(hSas);
            }
        }

        // 4. Simulate Ctrl+Alt+Del via keybd_event & SendInput
        keybd_event(VK_CONTROL, 0, 0, 0);
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_DELETE, 0, 0, 0);
        keybd_event(VK_DELETE, 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);

        // 5. Open Task Manager as responsive fallback if on user desktop
        ShellExecuteA(nullptr, "open", "taskmgr.exe", nullptr, nullptr, SW_SHOW);
    }

    void HandleSpecialKey(uint8_t keyCmd) {
        if (keyCmd == 1) { // Ctrl+Alt+Del Real SAS Trigger
            TriggerWindowsSAS();
        } else if (keyCmd == 2) { // Win+L Lock Screen
            LockWorkStation();
        } else if (keyCmd == 3) { // Task Manager
            ShellExecuteA(nullptr, "open", "taskmgr.exe", nullptr, nullptr, SW_SHOW);
        }
    }

    void HandleFileListRequest(const FileListRequest& req) {
        FileListResponseHeader resp{};
        std::string reqPath = req.path;
        if (reqPath.empty()) reqPath = "C:\\";
        snprintf(resp.path, sizeof(resp.path), "%s", reqPath.c_str());

        std::vector<FileItem> items;
        std::string searchPath = reqPath;
        if (searchPath.back() != '\\' && searchPath.back() != '/') searchPath += "\\";
        searchPath += "*";

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                FileItem item{};
                snprintf(item.name, sizeof(item.name), "%s", fd.cFileName);
                item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                item.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                item.lastModified = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                items.push_back(item);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }

        resp.itemCount = (uint32_t)items.size();

        std::vector<uint8_t> payload(sizeof(FileListResponseHeader) + items.size() * sizeof(FileItem));
        memcpy(payload.data(), &resp, sizeof(resp));
        if (!items.empty()) {
            memcpy(payload.data() + sizeof(resp), items.data(), items.size() * sizeof(FileItem));
        }

        EnterCriticalSection(&cs);
        NetworkHelper::SendPacket(clientSocket, MSG_FILE_LIST_RESP, payload.data(), payload.size(), &crypto, sequence++);
        LeaveCriticalSection(&cs);
    }

    void HandleFileDeleteRequest(const FileDeleteRequest& req) {
        DWORD attr = GetFileAttributesA(req.path);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryA(req.path);
            } else {
                DeleteFileA(req.path);
            }
        }
    }

    void HandleFileMkdirRequest(const FileMkdirRequest& req) {
        CreateDirectoryA(req.path, nullptr);
    }

    bool PerformHandshake() {
        Console::PrintInfo("Starting secure handshake...");

        // Initialize crypto
        if (!crypto.Initialize()) {
            Console::PrintError("Failed to initialize encryption");
            return false;
        }

        // Wait for handshake init from viewer
        PacketHeader header;
        std::vector<uint8_t> payload;
        
        if (!NetworkHelper::RecvPacket(clientSocket, header, payload, nullptr)) {
            Console::PrintError("Failed to receive handshake init");
            return false;
        }

        if (header.type != MSG_HANDSHAKE_INIT) {
            Console::PrintError("Unexpected message type");
            return false;
        }

        // Send our public key
        const auto& pubKey = crypto.GetPublicKey();
        ECDHKeyExchange keyEx;
        memset(&keyEx, 0, sizeof(keyEx));
        memcpy(keyEx.publicKey, pubKey.data(), std::min(pubKey.size(), sizeof(keyEx.publicKey)));
        keyEx.keyLength = (uint32_t)pubKey.size();

        if (!NetworkHelper::SendPacket(clientSocket, MSG_HANDSHAKE_PUBKEY, &keyEx, sizeof(keyEx))) {
            Console::PrintError("Failed to send public key");
            return false;
        }

        // Receive viewer's public key
        if (!NetworkHelper::RecvPacket(clientSocket, header, payload, nullptr)) {
            Console::PrintError("Failed to receive viewer public key");
            return false;
        }

        if (header.type != MSG_HANDSHAKE_PUBKEY || payload.size() < sizeof(ECDHKeyExchange)) {
            Console::PrintError("Invalid key exchange message");
            return false;
        }

        ECDHKeyExchange* viewerKey = (ECDHKeyExchange*)payload.data();

        // Derive shared secret
        if (!crypto.DeriveSharedSecret(viewerKey->publicKey, viewerKey->keyLength)) {
            Console::PrintError("Failed to derive shared secret");
            return false;
        }

        // Send handshake OK
        if (!NetworkHelper::SendPacket(clientSocket, MSG_HANDSHAKE_OK, nullptr, 0)) {
            Console::PrintError("Failed to send handshake OK");
            return false;
        }

        Console::PrintSuccess("Secure handshake completed!");
        return true;
    }

    bool HandleConnectionRequest() {
        PacketHeader header;
        std::vector<uint8_t> payload;

        if (!NetworkHelper::RecvPacket(clientSocket, header, payload, &crypto)) {
            return false;
        }

        if (header.type != MSG_CONNECT_REQUEST) {
            return false;
        }

        ConnectRequest* req = (ConnectRequest*)payload.data();
        connectedViewerName = req->viewerName;

        Console::SetColor(Console::BRIGHT_YELLOW);
        std::cout << "\n   +===================================================+\n";
        std::cout << "   |           INCOMING CONNECTION REQUEST             |\n";
        std::cout << "   +===================================================+\n";
        std::cout << "   |  From: " << std::setw(42) << std::left << connectedViewerName << " |\n";
        std::cout << "   |  IP:   " << std::setw(42) << std::left << connectedViewerIP << " |\n";
        std::cout << "   +===================================================+\n";
        Console::ResetColor();

        bool accept = false;

        if (g_config.unattendedAccess) {
            if (!g_config.unattendedPassword.empty()) {
                if (g_config.unattendedPassword == req->password) {
                    accept = true;
                } else {
                    Console::PrintWarning("Unattended connection rejected: Password mismatch!");
                    accept = false;
                }
            } else {
                accept = true;
            }
        } else {
            std::string msgText = "Incoming connection request from:\n\nViewer: " + connectedViewerName + "\nIP: " + connectedViewerIP + "\n\nDo you want to accept and allow remote control?";
            int res = MessageBoxA(g_hAgentWnd, msgText.c_str(), "Connection Approval Request", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
            accept = (res == IDYES);
        }

        if (accept) {
            if (!NetworkHelper::SendPacket(clientSocket, MSG_CONNECT_ACCEPT, nullptr, 0, &crypto)) {
                return false;
            }
            if (g_hAgentStatusLabel) {
                std::string statusStr = "Status: ● CONNECTED to Viewer (" + connectedViewerIP + ")";
                SetWindowTextA(g_hAgentStatusLabel, statusStr.c_str());
            }
            return true;
        } else {
            NetworkHelper::SendPacket(clientSocket, MSG_CONNECT_REJECT, nullptr, 0, &crypto);
            return false;
        }
    }

    static DWORD WINAPI CaptureThreadProc(LPVOID param) {
        AgentServer* self = (AgentServer*)param;
        self->CaptureLoop();
        return 0;
    }

    void CaptureLoop() {
        // High-precision timer resolution for locked 40 FPS
        HMODULE hWinmm = LoadLibraryA("winmm.dll");
        typedef UINT(WINAPI* PFN_tBP)(UINT);
        typedef UINT(WINAPI* PFN_tEP)(UINT);
        PFN_tBP fnTBP = hWinmm ? (PFN_tBP)GetProcAddress(hWinmm, "timeBeginPeriod") : nullptr;
        PFN_tEP fnTEP = hWinmm ? (PFN_tEP)GetProcAddress(hWinmm, "timeEndPeriod") : nullptr;
        if (fnTBP) fnTBP(1);

        int frameDelay = 1000 / g_config.targetFPS; // 25ms per frame for 40 FPS
        std::vector<uint8_t> jpegBuffer;
        int consecutiveErrors = 0;
        const int MAX_ERRORS = 10;
        DWORD lastClipCheck = 0;
        std::string lastAgentClip = "";

        Console::PrintInfo("Starting frame capture at " + std::to_string(g_config.targetFPS) + " FPS");

        while (running && connected) {
            DWORD startTime = GetTickCount();

            // Check local clipboard for changes
            if (GetTickCount() - lastClipCheck > 500) {
                lastClipCheck = GetTickCount();
                std::string currentClip = ClipboardHelper::GetText();
                if (!currentClip.empty() && currentClip != lastAgentClip && currentClip.size() < 1000000) {
                    lastAgentClip = currentClip;
                    EnterCriticalSection(&cs);
                    NetworkHelper::SendPacket(clientSocket, MSG_CLIPBOARD_TEXT, (const void*)currentClip.data(), currentClip.size(), &crypto, sequence++);
                    LeaveCriticalSection(&cs);
                }
            }

            // Combined capture and encode with screen diff detection
            bool changed = true;
            if (!capture.CaptureAndEncode(jpegBuffer, g_config.jpegQuality, &changed)) {
                consecutiveErrors++;
                if (consecutiveErrors >= MAX_ERRORS) {
                    Console::PrintError("Too many capture errors, stopping");
                    break;
                }
                Sleep(100);  // Brief pause before retry
                continue;
            }
            consecutiveErrors = 0;  // Reset on success

            if (!changed) {
                // Screen unchanged - skip encoding & network send for low-spec PC optimization!
                DWORD elapsed = GetTickCount() - startTime;
                if ((int)elapsed < frameDelay) {
                    Sleep(frameDelay - elapsed);
                }
                continue;
            }

            // Prepare frame header
            FrameHeader fh;
            fh.frameId = frameId++;
            fh.dataSize = (uint32_t)jpegBuffer.size();
            fh.width = (uint16_t)capture.GetWidth();
            fh.height = (uint16_t)capture.GetHeight();
            fh.quality = (uint8_t)g_config.jpegQuality;
            fh.scale = capture.GetScale();

            // Combine header and data
            std::vector<uint8_t> packet(sizeof(FrameHeader) + jpegBuffer.size());
            memcpy(packet.data(), &fh, sizeof(FrameHeader));
            memcpy(packet.data() + sizeof(FrameHeader), jpegBuffer.data(), jpegBuffer.size());

            // Send encrypted frame with retry
            EnterCriticalSection(&cs);
            bool sent = NetworkHelper::SendPacket(clientSocket, MSG_FRAME_DATA, 
                                                  packet.data(), packet.size(), &crypto, sequence++);
            LeaveCriticalSection(&cs);

            if (!sent) {
                consecutiveErrors++;
                if (consecutiveErrors >= MAX_ERRORS) {
                    Console::PrintError("Too many send failures, disconnecting");
                    break;
                }
                Sleep(50);
                continue;
            }
            consecutiveErrors = 0;

            // Precise 40 FPS frame delay control
            DWORD elapsed = GetTickCount() - startTime;
            if ((int)elapsed < frameDelay) {
                Sleep(frameDelay - elapsed);
            }
        }

        if (fnTEP) fnTEP(1);
        if (hWinmm) FreeLibrary(hWinmm);
        connected = false;
        Console::PrintInfo("Capture loop ended");
    }

    static DWORD WINAPI ControlThreadProc(LPVOID param) {
        AgentServer* self = (AgentServer*)param;
        self->ControlLoop();
        return 0;
    }

    void ControlLoop() {
        Console::PrintInfo("Control receiver started");
        int errorCount = 0;
        const int MAX_RECV_ERRORS = 5;

        while (running && connected) {
            PacketHeader header;
            std::vector<uint8_t> payload;

            if (!NetworkHelper::RecvPacket(clientSocket, header, payload, &crypto)) {
                errorCount++;
                if (errorCount >= MAX_RECV_ERRORS) {
                    Console::PrintWarning("Connection appears lost");
                    break;
                }
                Sleep(100);  // Brief pause before retry
                continue;
            }
            errorCount = 0;  // Reset on successful recv

            if (header.type == MSG_CONTROL_MOUSE && payload.size() >= sizeof(ControlPacket)) {
                ControlPacket* pkt = (ControlPacket*)payload.data();
                input.HandleMouse(*pkt);
            }
            else if (header.type == MSG_CONTROL_KEYBOARD && payload.size() >= sizeof(ControlPacket)) {
                ControlPacket* pkt = (ControlPacket*)payload.data();
                input.HandleKeyboard(*pkt);
            }
            else if (header.type == MSG_CLIPBOARD_TEXT && !payload.empty()) {
                std::string clipText((char*)payload.data(), payload.size());
                ClipboardHelper::SetText(clipText);
                Console::PrintInfo("Updated clipboard from Viewer");
            }
            else if (header.type == MSG_PING) {
                NetworkHelper::SendPacket(clientSocket, MSG_PONG, nullptr, 0, &crypto);
            }
            else if (header.type == MSG_FILE_TRANSFER_REQ && payload.size() >= sizeof(FileTransferRequest)) {
                const FileTransferRequest* req = (const FileTransferRequest*)payload.data();
                FileTransferResponse resp{};
                resp.transferId = req->transferId;
                resp.accepted = 1;
                resp.resumeOffset = 0;

                FileTransferBegin begin{};
                begin.transferId = req->transferId;
                begin.fileSize = req->fileSize;
                begin.chunkSize = req->chunkSize;
                memcpy(begin.fileName, req->fileName, sizeof(begin.fileName));
                HandleFileTransferBegin(begin);

                EnterCriticalSection(&cs);
                NetworkHelper::SendPacket(clientSocket, MSG_FILE_TRANSFER_ACK, &resp, sizeof(resp), &crypto, sequence++);
                LeaveCriticalSection(&cs);
            }
            else if (header.type == MSG_FILE_TRANSFER_BEGIN && payload.size() >= sizeof(FileTransferBegin)) {
                const FileTransferBegin* begin = (const FileTransferBegin*)payload.data();
                HandleFileTransferBegin(*begin);
            }
            else if (header.type == MSG_FILE_TRANSFER_CHUNK && payload.size() >= sizeof(FileTransferChunkHeader)) {
                const FileTransferChunkHeader* chunkHeader = (const FileTransferChunkHeader*)payload.data();
                const uint8_t* chunkData = payload.data() + sizeof(FileTransferChunkHeader);
                size_t chunkSize = payload.size() - sizeof(FileTransferChunkHeader);

                if (chunkHeader->chunkSize <= chunkSize) {
                    HandleFileTransferChunk(*chunkHeader, chunkData, chunkHeader->chunkSize);
                    FileTransferChunkAck ack{};
                    ack.transferId = chunkHeader->transferId;
                    ack.chunkIndex = chunkHeader->chunkIndex;
                    ack.status = 1;

                    EnterCriticalSection(&cs);
                    NetworkHelper::SendPacket(clientSocket, MSG_FILE_TRANSFER_CHUNK_ACK, &ack, sizeof(ack), &crypto, sequence++);
                    LeaveCriticalSection(&cs);
                }
            }
            else if (header.type == MSG_FILE_TRANSFER_END && payload.size() >= sizeof(FileTransferEnd)) {
                const FileTransferEnd* end = (const FileTransferEnd*)payload.data();
                HandleFileTransferEnd(*end);
            }
            else if (header.type == MSG_CHAT_TEXT && !payload.empty()) {
                std::string* pChatMsg = new std::string((char*)payload.data(), payload.size());
                if (g_hAgentWnd) {
                    PostMessageA(g_hAgentWnd, WM_APP + 10, 0, (LPARAM)pChatMsg);
                } else {
                    delete pChatMsg;
                }
            }
            else if (header.type == MSG_SPECIAL_KEY && payload.size() >= sizeof(SpecialKeyPacket)) {
                SpecialKeyPacket* pkt = (SpecialKeyPacket*)payload.data();
                HandleSpecialKey(pkt->keyCmd);
            }
            else if (header.type == MSG_FILE_LIST_REQ && payload.size() >= sizeof(FileListRequest)) {
                FileListRequest* req = (FileListRequest*)payload.data();
                HandleFileListRequest(*req);
            }
            else if (header.type == MSG_FILE_DELETE_REQ && payload.size() >= sizeof(FileDeleteRequest)) {
                FileDeleteRequest* req = (FileDeleteRequest*)payload.data();
                HandleFileDeleteRequest(*req);
            }
            else if (header.type == MSG_FILE_MKDIR_REQ && payload.size() >= sizeof(FileMkdirRequest)) {
                FileMkdirRequest* req = (FileMkdirRequest*)payload.data();
                HandleFileMkdirRequest(*req);
            }
            else if (header.type == MSG_DISCONNECT) {
                Console::PrintInfo("Viewer disconnected gracefully");
                break;
            }
        }

        connected = false;
        input.ReleaseAllInputs();
        CleanupFileTransfers();
        Console::PrintInfo("Control loop ended");
    }

    void WaitForConnections() {
        while (running) {
            Console::SetColor(Console::BRIGHT_GREEN);
            std::cout << "\n   ---------------------------------------------------\n";
            std::cout << "   Waiting for incoming connections...\n";
            std::cout << "   Press Ctrl+C to stop the server\n";
            std::cout << "   ---------------------------------------------------\n\n";
            Console::ResetColor();

            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);

            if (clientSocket == INVALID_SOCKET) {
                if (running) Console::PrintError("Accept failed");
                continue;
            }

            const char* ip = inet_ntoa(clientAddr.sin_addr);
            connectedViewerIP = ip ? ip : "0.0.0.0";

            Console::PrintSuccess("Viewer connected from: " + connectedViewerIP);

            // Check IP whitelist
            if (!g_config.allowedViewerIP.empty() && g_config.allowedViewerIP != connectedViewerIP) {
                Console::PrintWarning("Connection rejected - IP not in whitelist");
                closesocket(clientSocket);
                clientSocket = INVALID_SOCKET;
                continue;
            }

            NetworkHelper::SetSocketNoDelay(clientSocket);
            NetworkHelper::SetSocketKeepalive(clientSocket);
            NetworkHelper::SetSocketBuffers(clientSocket);
            NetworkHelper::SetSocketTimeout(clientSocket, 10000, 10000);

            // Perform secure handshake
            if (!PerformHandshake()) {
                closesocket(clientSocket);
                clientSocket = INVALID_SOCKET;
                crypto.Cleanup();
                continue;
            }

            // Handle connection request
            if (!HandleConnectionRequest()) {
                closesocket(clientSocket);
                clientSocket = INVALID_SOCKET;
                crypto.Cleanup();
                continue;
            }

            connected = true;

            // Start capture and control threads
            captureThread = CreateThread(nullptr, 0, CaptureThreadProc, this, 0, nullptr);
            controlThread = CreateThread(nullptr, 0, ControlThreadProc, this, 0, nullptr);

            // Wait for disconnection
            WaitForSingleObject(captureThread, INFINITE);
            WaitForSingleObject(controlThread, INFINITE);

            CloseHandle(captureThread);
            CloseHandle(controlThread);
            captureThread = nullptr;
            controlThread = nullptr;

            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            crypto.Cleanup();

            Console::PrintInfo("Session ended. Ready for new connections.");
        }
    }
};

// ============================================================================
// CONFIGURATION MENU
// ============================================================================

// ============================================================================
// GRAPHICAL GUI AGENT CONTROL DASHBOARD
// ============================================================================

#define IDC_AGENT_PORT        2001
#define IDC_AGENT_SCALE       2002
#define IDC_AGENT_UNATTEND    2005
#define IDC_AGENT_BTN_START   2006
#define IDC_AGENT_BTN_DISC    2007
#define IDC_AGENT_PASS        2008
#define IDC_AGENT_BTN_CHAT    2009

#define IDC_AGENT_CHAT_LIST   2101
#define IDC_AGENT_CHAT_INPUT  2102
#define IDC_AGENT_CHAT_SEND   2103

static HWND g_hAgentPortEdit = nullptr;
static HWND g_hAgentScaleCombo = nullptr;
static HWND g_hAgentUnattendCheck = nullptr;
static HWND g_hAgentPassEdit = nullptr;
static HWND g_hAgentBtnStart = nullptr;
static HWND g_hAgentBtnDisc = nullptr;
static HWND g_hAgentBtnChat = nullptr;
static HWND g_hAgentChatInput = nullptr;
static AgentServer* g_pGuiAgentServer = nullptr;
static HANDLE g_hAgentThread = nullptr;
static bool g_agentRunning = false;

static LRESULT CALLBACK AgentChatProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hAgentChatList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL, 15, 15, 410, 190, hwnd, (HMENU)IDC_AGENT_CHAT_LIST, nullptr, nullptr);
        SendMessage(g_hAgentChatList, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentChatInput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 15, 215, 310, 28, hwnd, (HMENU)IDC_AGENT_CHAT_INPUT, nullptr, nullptr);
        SendMessage(g_hAgentChatInput, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hSendBtn = CreateWindowA("BUTTON", "Send",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 335, 215, 90, 28, hwnd, (HMENU)IDC_AGENT_CHAT_SEND, nullptr, nullptr);
        SendMessage(hSendBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_AGENT_CHAT_SEND) {
            char textBuf[512] = {0};
            GetWindowTextA(g_hAgentChatInput, textBuf, sizeof(textBuf));
            if (strlen(textBuf) > 0) {
                std::string msgStr = textBuf;
                SetWindowTextA(g_hAgentChatInput, "");
                std::string myEntry = "[Me]: " + msgStr;
                SendMessageA(g_hAgentChatList, LB_ADDSTRING, 0, (LPARAM)myEntry.c_str());
                SendMessage(g_hAgentChatList, WM_VSCROLL, SB_BOTTOM, 0);

                if (g_pGuiAgentServer) {
                    g_pGuiAgentServer->SendChatMessage(msgStr);
                }
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        g_hAgentChatWnd = nullptr;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ShowAgentChatWindow() {
    if (g_hAgentChatWnd) {
        SetForegroundWindow(g_hAgentChatWnd);
        return;
    }
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = AgentChatProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RCAgentChatClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    g_hAgentChatWnd = CreateWindowA(
        wc.lpszClassName,
        "Encrypted Live Chat - Remote Controller Agent",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 455, 295,
        g_hAgentWnd, nullptr, hInstance, nullptr
    );

    if (g_hAgentChatWnd) {
        ShowWindow(g_hAgentChatWnd, SW_SHOW);
        UpdateWindow(g_hAgentChatWnd);
    }
}

static DWORD WINAPI AgentWorkerThreadProc(LPVOID param) {
    if (g_pGuiAgentServer) {
        g_pGuiAgentServer->WaitForConnections();
    }
    return 0;
}

static LRESULT CALLBACK GuiAgentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hTitle = CreateWindowA("STATIC", "Remote Controller Agent v2.0 - Server Control",
            WS_CHILD | WS_VISIBLE, 20, 15, 440, 25, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hTitle, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentStatusLabel = CreateWindowA("STATIC", "Status: Ready to Start Server",
            WS_CHILD | WS_VISIBLE, 20, 45, 440, 25, hwnd, nullptr, nullptr, nullptr);
        SendMessage(g_hAgentStatusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hPortLabel = CreateWindowA("STATIC", "Listen Port:",
            WS_CHILD | WS_VISIBLE, 20, 80, 90, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hPortLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "5000",
            WS_CHILD | WS_VISIBLE | ES_NUMBER, 110, 78, 80, 24, hwnd, (HMENU)IDC_AGENT_PORT, nullptr, nullptr);
        SendMessage(g_hAgentPortEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hScaleLabel = CreateWindowA("STATIC", "Quality Scale:",
            WS_CHILD | WS_VISIBLE, 210, 80, 90, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hScaleLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentScaleCombo = CreateWindowA("COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 305, 78, 145, 120, hwnd, (HMENU)IDC_AGENT_SCALE, nullptr, nullptr);
        SendMessage(g_hAgentScaleCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(g_hAgentScaleCombo, CB_ADDSTRING, 0, (LPARAM)"Low (25%)");
        SendMessageA(g_hAgentScaleCombo, CB_ADDSTRING, 0, (LPARAM)"Medium (50%)");
        SendMessageA(g_hAgentScaleCombo, CB_ADDSTRING, 0, (LPARAM)"High (75%)");
        SendMessageA(g_hAgentScaleCombo, CB_ADDSTRING, 0, (LPARAM)"Full (100%)");
        SendMessage(g_hAgentScaleCombo, CB_SETCURSEL, 1, 0);

        g_hAgentUnattendCheck = CreateWindowA("BUTTON", "Enable Unattended Access",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 118, 190, 25, hwnd, (HMENU)IDC_AGENT_UNATTEND, nullptr, nullptr);
        SendMessage(g_hAgentUnattendCheck, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hPassLabel = CreateWindowA("STATIC", "Password:",
            WS_CHILD | WS_VISIBLE, 220, 120, 70, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hPassLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentPassEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_PASSWORD, 295, 118, 155, 24, hwnd, (HMENU)IDC_AGENT_PASS, nullptr, nullptr);
        SendMessage(g_hAgentPassEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentBtnStart = CreateWindowA("BUTTON", "Start Agent Server",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 160, 430, 35, hwnd, (HMENU)IDC_AGENT_BTN_START, nullptr, nullptr);
        SendMessage(g_hAgentBtnStart, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentBtnChat = CreateWindowA("BUTTON", "Open Live Chat",
            WS_CHILD | WS_VISIBLE, 20, 205, 205, 30, hwnd, (HMENU)IDC_AGENT_BTN_CHAT, nullptr, nullptr);
        SendMessage(g_hAgentBtnChat, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hAgentBtnDisc = CreateWindowA("BUTTON", "Disconnect Viewer",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 245, 205, 205, 30, hwnd, (HMENU)IDC_AGENT_BTN_DISC, nullptr, nullptr);
        SendMessage(g_hAgentBtnDisc, WM_SETFONT, (WPARAM)hFont, TRUE);

        std::string devId = "Device ID: " + DeviceID::Generate() + "  |  Local IP: " + NetworkHelper::GetLocalIP();
        HWND hFooter = CreateWindowA("STATIC", devId.c_str(),
            WS_CHILD | WS_VISIBLE, 20, 250, 430, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hFooter, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDC_AGENT_BTN_START) {
            if (!g_agentRunning) {
                char portBuf[32] = {0};
                GetWindowTextA(g_hAgentPortEdit, portBuf, sizeof(portBuf));
                try { g_config.port = (uint16_t)std::stoi(portBuf); } catch (...) { g_config.port = 5000; }

                char passBuf[64] = {0};
                GetWindowTextA(g_hAgentPassEdit, passBuf, sizeof(passBuf));
                g_config.unattendedPassword = passBuf;

                int scaleIndex = (int)SendMessage(g_hAgentScaleCombo, CB_GETCURSEL, 0, 0);
                switch (scaleIndex) {
                    case 0: g_config.captureScale = 0.25f; break;
                    case 1: g_config.captureScale = 0.50f; break;
                    case 2: g_config.captureScale = 0.75f; break;
                    case 3: g_config.captureScale = 1.00f; break;
                    default: g_config.captureScale = 0.50f; break;
                }

                g_config.unattendedAccess = (SendMessage(g_hAgentUnattendCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_config.deviceId = DeviceID::Generate();

                g_pGuiAgentServer = new AgentServer();
                if (g_pGuiAgentServer->Start(g_config)) {
                    g_hAgentThread = CreateThread(nullptr, 0, AgentWorkerThreadProc, nullptr, 0, nullptr);
                    g_agentRunning = true;
                    SetWindowTextA(g_hAgentStatusLabel, "Status: LISTENING for viewers on port ");
                    SetWindowTextA(g_hAgentBtnStart, "Stop Agent Server");
                    EnableWindow(g_hAgentBtnDisc, TRUE);
                } else {
                    delete g_pGuiAgentServer;
                    g_pGuiAgentServer = nullptr;
                    MessageBoxA(hwnd, "Failed to bind port. Check if another instance is running.", "Agent Error", MB_ICONERROR);
                }
            } else {
                if (g_pGuiAgentServer) {
                    g_pGuiAgentServer->Stop();
                    if (g_hAgentThread) {
                        WaitForSingleObject(g_hAgentThread, 2000);
                        CloseHandle(g_hAgentThread);
                        g_hAgentThread = nullptr;
                    }
                    delete g_pGuiAgentServer;
                    g_pGuiAgentServer = nullptr;
                }
                g_agentRunning = false;
                SetWindowTextA(g_hAgentStatusLabel, "Status: Stopped");
                SetWindowTextA(g_hAgentBtnStart, "Start Agent Server");
                EnableWindow(g_hAgentBtnDisc, FALSE);
            }
            return 0;
        }

        if (id == IDC_AGENT_BTN_DISC) {
            if (g_pGuiAgentServer) {
                g_pGuiAgentServer->DisconnectClient();
            }
            return 0;
        }

        if (id == IDC_AGENT_BTN_CHAT) {
            ShowAgentChatWindow();
            return 0;
        }
        break;
    }

    case WM_APP + 10: {
        std::string* pMsg = (std::string*)lParam;
        if (pMsg) {
            if (!g_hAgentChatWnd) {
                ShowAgentChatWindow();
            }
            if (g_hAgentChatList) {
                std::string entry = "[Viewer]: " + *pMsg;
                SendMessageA(g_hAgentChatList, LB_ADDSTRING, 0, (LPARAM)entry.c_str());
                SendMessage(g_hAgentChatList, WM_VSCROLL, SB_BOTTOM, 0);
            }
            delete pMsg;
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_pGuiAgentServer) {
            g_pGuiAgentServer->Stop();
            delete g_pGuiAgentServer;
            g_pGuiAgentServer = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ShowGuiAgentDashboard() {
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = GuiAgentProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RCAgentDashboardClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 485;
    int winH = 320;
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    HWND hwnd = CreateWindowA(
        wc.lpszClassName,
        "Remote Controller Agent v2.0",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return;
    g_hAgentWnd = hwnd;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (g_config.unattendedAccess || !g_config.allowedViewerIP.empty()) {
        SendMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDC_AGENT_BTN_START, 0), 0);
    }

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    EnableDPIAwareness();

    std::string cmd(lpCmdLine ? lpCmdLine : "");
    if (cmd.find("--unattended") != std::string::npos) {
        g_config.unattendedAccess = true;
    }
    size_t ipPos = cmd.find("--viewer-ip");
    if (ipPos != std::string::npos) {
        std::istringstream iss(cmd.substr(ipPos + 11));
        iss >> g_config.allowedViewerIP;
    }

    ShowGuiAgentDashboard();
    return 0;
}

int main() {
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOW);
}

