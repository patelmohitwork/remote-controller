/*
 * Remote Controller - Common Protocol & Encryption Header
 * Uses key exchange + AES-256 encryption
 * Windows CryptoAPI for cryptography
 */

#pragma once
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <wincrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(__has_include)
#if __has_include(<mstcpip.h>)
#include <mstcpip.h>
#endif
#endif
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Advapi32.lib")

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
#endif

#ifndef ALG_SID_SHA_256
#define ALG_SID_SHA_256 12
#endif

#ifndef CALG_SHA_256
#define CALG_SHA_256 (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_256)
#endif

// ============================================================================
// CONSOLE UI HELPERS
// ============================================================================

namespace Console {
    enum Color {
        BLACK = 0, BLUE = 1, GREEN = 2, CYAN = 3, RED = 4, MAGENTA = 5,
        YELLOW = 6, WHITE = 7, GRAY = 8, BRIGHT_BLUE = 9, BRIGHT_GREEN = 10,
        BRIGHT_CYAN = 11, BRIGHT_RED = 12, BRIGHT_MAGENTA = 13, BRIGHT_YELLOW = 14, BRIGHT_WHITE = 15
    };

    inline void InitConsole() {
        // Enable UTF-8 output
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        // Enable ANSI/VT100 sequences
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hOut, &mode);
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    inline void SetColor(Color fg, Color bg = BLACK) {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)(fg | (bg << 4)));
    }

    inline void ResetColor() {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), WHITE);
    }

    inline void ClearScreen() {
        system("cls");
    }

    inline void SetTitle(const char* title) {
        SetConsoleTitleA(title);
    }

    inline void PrintLogo() {
        // Logo banner removed
    }

    inline void PrintBox(const std::string& title, const std::string& content) {
        SetColor(BRIGHT_WHITE);
        std::cout << "\n   +";
        for (size_t i = 0; i < title.length() + 4; i++) std::cout << "-";
        std::cout << "+\n";
        std::cout << "   |  " << title << "  |\n";
        std::cout << "   +";
        for (size_t i = 0; i < title.length() + 4; i++) std::cout << "-";
        std::cout << "+\n";
        ResetColor();
        SetColor(GRAY);
        std::cout << "   " << content << "\n";
        ResetColor();
    }

    inline void PrintStatus(const std::string& msg, Color color = GREEN) {
        SetColor(BRIGHT_WHITE);
        std::cout << "   [";
        SetColor(color);
        std::cout << "*";
        SetColor(BRIGHT_WHITE);
        std::cout << "] ";
        ResetColor();
        std::cout << msg << std::endl;
    }

    inline void PrintError(const std::string& msg) {
        PrintStatus(msg, RED);
    }

    inline void PrintSuccess(const std::string& msg) {
        PrintStatus(msg, GREEN);
    }

    inline void PrintInfo(const std::string& msg) {
        PrintStatus(msg, CYAN);
    }

    inline void PrintWarning(const std::string& msg) {
        PrintStatus(msg, YELLOW);
    }

    inline std::string GetInput(const std::string& prompt) {
        SetColor(BRIGHT_YELLOW);
        std::cout << "\n   > " << prompt << ": ";
        SetColor(BRIGHT_WHITE);
        std::string input;
        std::getline(std::cin, input);
        ResetColor();
        return input;
    }

    inline int GetMenuChoice(const std::vector<std::string>& options) {
        SetColor(BRIGHT_WHITE);
        std::cout << "\n   +======================================+\n";
        std::cout << "   |          SELECT OPTION               |\n";
        std::cout << "   +======================================+\n";
        for (size_t i = 0; i < options.size(); i++) {
            std::cout << "   |  ";
            SetColor(BRIGHT_CYAN);
            std::cout << "[" << (i + 1) << "]";
            SetColor(BRIGHT_WHITE);
            std::cout << " " << options[i];
            for (size_t j = options[i].length(); j < 30; j++) std::cout << " ";
            std::cout << "|\n";
        }
        std::cout << "   +======================================+\n";
        ResetColor();

        std::string input = GetInput("Enter choice");
        try {
            return std::stoi(input);
        } catch (...) {
            return -1;
        }
    }
}

// ============================================================================
// PROTOCOL DEFINITIONS
// ============================================================================

#define RC_VERSION          "2.0.0"
#define RC_DEFAULT_PORT     5000
#define RC_CONTROL_PORT     5001
#define RC_MAGIC            0x52435043  // "RCPC"

// Message types
enum MessageType : uint8_t {
    MSG_HANDSHAKE_INIT      = 0x01,  // Viewer -> Agent: Start handshake
    MSG_HANDSHAKE_PUBKEY    = 0x02,  // Exchange ECDH public keys
    MSG_HANDSHAKE_VERIFY    = 0x03,  // Verify shared secret
    MSG_HANDSHAKE_OK        = 0x04,  // Handshake complete
    MSG_CONNECT_REQUEST     = 0x10,  // Viewer requests connection
    MSG_CONNECT_ACCEPT      = 0x11,  // Agent accepts
    MSG_CONNECT_REJECT      = 0x12,  // Agent rejects
    MSG_FRAME_DATA          = 0x20,  // Encrypted frame data
    MSG_CONTROL_MOUSE       = 0x30,  // Mouse event
    MSG_CONTROL_KEYBOARD    = 0x31,  // Keyboard event
    MSG_SPECIAL_KEY         = 0x32,  // Ctrl+Alt+Del, Win+L, TaskMgr
    MSG_CLIPBOARD_TEXT      = 0x35,  // Encrypted clipboard text exchange
    MSG_CHAT_TEXT           = 0x36,  // Encrypted live text chat
    MSG_PING                = 0x40,
    MSG_PONG                = 0x41,
    MSG_FILE_TRANSFER_BEGIN = 0x50,  // File transfer start
    MSG_FILE_TRANSFER_CHUNK = 0x51,  // File transfer data chunk
    MSG_FILE_TRANSFER_END   = 0x52,  // File transfer complete
    MSG_FILE_TRANSFER_REQ   = 0x53,  // Pre-transfer handshake request
    MSG_FILE_TRANSFER_ACK   = 0x54,  // Pre-transfer handshake response
    MSG_FILE_TRANSFER_CHUNK_ACK = 0x55,// Chunk verification ACK
    MSG_FILE_LIST_REQ       = 0x56,  // Request directory listing
    MSG_FILE_LIST_RESP      = 0x57,  // Directory listing response
    MSG_FILE_DELETE_REQ     = 0x58,  // Remote file/folder deletion
    MSG_FILE_MKDIR_REQ      = 0x59,  // Remote directory creation
    MSG_DISCONNECT          = 0xFF
};

#pragma pack(push, 1)

// Base packet header
struct PacketHeader {
    uint32_t magic;       // RC_MAGIC
    uint8_t  type;        // MessageType
    uint32_t length;      // Payload length
    uint32_t sequence;    // Packet sequence number
};

// Handshake init message
struct HandshakeInit {
    char     version[16];      // Version string
    char     deviceId[32];     // Unique device ID
    uint8_t  unattendedMode;   // 0 = require approval, 1 = auto-accept
};

// Handshake key material exchange
struct ECDHKeyExchange {
    uint8_t  publicKey[128];   // Random key material for shared key derivation
    uint32_t keyLength;
};

// Connection request (from viewer)
struct ConnectRequest {
    char     viewerName[64];   // Name/description of viewer
    uint32_t requestedFPS;     // Requested framerate
    uint8_t  quality;          // JPEG quality (1-100)
    char     password[64];     // Unattended access password
};

// Control packet (mouse/keyboard)
struct ControlPacket {
    uint8_t  kind;        // 1 = mouse, 2 = keyboard
    uint8_t  mouseType;   // 1=move, 2=Ldown, 3=Lup, 4=Rdown, 5=Rup, 6=scroll
    int32_t  x;           // Mouse X (in image coords)
    int32_t  y;           // Mouse Y
    int16_t  scrollDelta; // Mouse scroll delta
    uint16_t vk;          // Virtual key code
    uint8_t  keyDown;     // 1 = down, 0 = up
};

// Frame header
struct FrameHeader {
    uint32_t frameId;
    uint32_t dataSize;    // Size of JPEG data
    uint16_t width;       // Original width
    uint16_t height;      // Original height
    uint8_t  quality;     // JPEG quality used
    float    scale;       // Scale factor used
};

struct FileTransferBegin {
    uint32_t transferId;
    uint64_t fileSize;
    uint32_t chunkSize;
    char     fileName[260];
};

struct FileTransferChunkHeader {
    uint32_t transferId;
    uint32_t chunkIndex;
    uint32_t chunkSize;
};

struct FileTransferEnd {
    uint32_t transferId;
    uint32_t status;
};

struct FileTransferRequest {
    uint32_t transferId;
    uint64_t fileSize;
    uint32_t chunkSize;
    uint32_t totalChunks;
    uint32_t checksum;
    char     fileName[260];
};

struct FileTransferResponse {
    uint32_t transferId;
    uint8_t  accepted;   // 1 = accept, 0 = reject
    uint64_t resumeOffset;
};

struct FileTransferChunkAck {
    uint32_t transferId;
    uint32_t chunkIndex;
    uint8_t  status;     // 1 = OK, 0 = Fail
};

struct SpecialKeyPacket {
    uint8_t keyCmd;      // 1 = Ctrl+Alt+Del, 2 = Win+L, 3 = TaskMgr
};

struct FileListRequest {
    char path[260];
};

struct FileItem {
    char name[260];
    uint64_t size;
    uint8_t isDir;
    uint64_t lastModified;
};

struct FileListResponseHeader {
    char path[260];
    uint32_t itemCount;
};

struct FileDeleteRequest {
    char path[260];
};

struct FileMkdirRequest {
    char path[260];
};

#pragma pack(pop)

// ============================================================================
// ENCRYPTION (Windows CryptoAPI)
// ============================================================================

class SecureChannel {
private:
    HCRYPTPROV hProv = 0;
    std::vector<uint8_t> publicKey;
    std::vector<uint8_t> aesKey;
    bool initialized = false;

    bool BuildSessionKey(const uint8_t* iv, HCRYPTKEY& hKey) {
        hKey = 0;
        if (hProv == 0 || aesKey.empty() || !iv) return false;

        HCRYPTHASH hHash = 0;
        if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            return false;
        }

        bool ok = CryptHashData(hHash, aesKey.data(), (DWORD)aesKey.size(), 0) == TRUE;
        DWORD flags = CRYPT_EXPORTABLE | (256 << 16);

        if (ok) {
            ok = CryptDeriveKey(hProv, CALG_AES_256, hHash, flags, &hKey) == TRUE;
        }

        if (ok) {
            DWORD mode = CRYPT_MODE_CBC;
            ok = CryptSetKeyParam(hKey, KP_MODE, (BYTE*)&mode, 0) == TRUE;
        }

        if (ok) {
            ok = CryptSetKeyParam(hKey, KP_IV, (BYTE*)iv, 0) == TRUE;
        }

        CryptDestroyHash(hHash);

        if (!ok && hKey) {
            CryptDestroyKey(hKey);
            hKey = 0;
        }
        return ok;
    }

public:
    SecureChannel() {}

    ~SecureChannel() {
        Cleanup();
    }

    void Cleanup() {
        if (hProv != 0) {
            CryptReleaseContext(hProv, 0);
            hProv = 0;
        }
        publicKey.clear();
        aesKey.clear();
        initialized = false;
    }

    bool Initialize() {
        Cleanup();

        if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            Console::PrintError("Failed to initialize CryptoAPI context");
            return false;
        }

        publicKey.resize(32);
        if (!CryptGenRandom(hProv, (DWORD)publicKey.size(), publicKey.data())) {
            Console::PrintError("Failed to generate local key material");
            return false;
        }

        initialized = true;
        return true;
    }

    const std::vector<uint8_t>& GetPublicKey() const {
        return publicKey;
    }

    bool DeriveSharedSecret(const uint8_t* peerPublicKey, uint32_t peerKeyLen) {
        if (!initialized) return false;
        if (!peerPublicKey || peerKeyLen == 0 || peerKeyLen > 128) return false;

        std::vector<uint8_t> peer(peerPublicKey, peerPublicKey + peerKeyLen);

        std::vector<uint8_t> material;
        material.reserve(publicKey.size() + peer.size());

        // Stable ordering ensures both peers derive identical keys.
        if (std::lexicographical_compare(peer.begin(), peer.end(), publicKey.begin(), publicKey.end())) {
            material.insert(material.end(), peer.begin(), peer.end());
            material.insert(material.end(), publicKey.begin(), publicKey.end());
        } else {
            material.insert(material.end(), publicKey.begin(), publicKey.end());
            material.insert(material.end(), peer.begin(), peer.end());
        }

        HCRYPTHASH hHash = 0;
        if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            Console::PrintError("Failed to create session hash");
            return false;
        }

        bool ok = CryptHashData(hHash, material.data(), (DWORD)material.size(), 0) == TRUE;
        aesKey.resize(32);
        DWORD outLen = (DWORD)aesKey.size();
        if (ok) {
            ok = CryptGetHashParam(hHash, HP_HASHVAL, aesKey.data(), &outLen, 0) == TRUE;
        }
        CryptDestroyHash(hHash);

        if (!ok || outLen < 32) {
            Console::PrintError("Failed to derive encryption key");
            return false;
        }

        Console::PrintSuccess("Secure channel established (CryptoAPI AES-256)");
        return true;
    }

    std::vector<uint8_t> Encrypt(const uint8_t* data, size_t len) {
        if (hProv == 0 || aesKey.empty() || (!data && len > 0)) return {};

        uint8_t iv[16] = {0};
        if (!CryptGenRandom(hProv, sizeof(iv), iv)) {
            return {};
        }

        HCRYPTKEY hKey = 0;
        if (!BuildSessionKey(iv, hKey)) {
            return {};
        }

        DWORD plainLen = (DWORD)len;
        DWORD bufLen = plainLen + 16;
        std::vector<uint8_t> cipher(bufLen);
        if (len > 0) {
            memcpy(cipher.data(), data, len);
        }

        BOOL ok = CryptEncrypt(hKey, 0, TRUE, 0, cipher.data(), &plainLen, bufLen);
        CryptDestroyKey(hKey);
        if (!ok) {
            return {};
        }

        cipher.resize(plainLen);

        std::vector<uint8_t> result(16 + cipher.size());
        memcpy(result.data(), iv, 16);
        if (!cipher.empty()) {
            memcpy(result.data() + 16, cipher.data(), cipher.size());
        }
        return result;
    }

    std::vector<uint8_t> Decrypt(const uint8_t* data, size_t len) {
        if (hProv == 0 || aesKey.empty() || !data || len < 16) return {};

        HCRYPTKEY hKey = 0;
        if (!BuildSessionKey(data, hKey)) {
            return {};
        }

        size_t cipherLen = len - 16;
        std::vector<uint8_t> result(cipherLen);
        if (cipherLen > 0) {
            memcpy(result.data(), data + 16, cipherLen);
        }

        DWORD plainLen = (DWORD)cipherLen;
        BOOL ok = CryptDecrypt(hKey, 0, TRUE, 0, result.data(), &plainLen);
        CryptDestroyKey(hKey);
        if (!ok) {
            return {};
        }

        result.resize(plainLen);
        return result;
    }

    bool IsReady() const { return initialized && !aesKey.empty(); }
};

// ============================================================================
// CONTACT STORAGE (IP Address Book)
// ============================================================================

struct Contact {
    std::string name;
    std::string ip;
    uint16_t port;
    std::string deviceId;
    time_t lastConnected;
    bool unattendedEnabled;
};

class ContactManager {
private:
    std::map<std::string, Contact> contacts;
    std::string filePath;

public:
    ContactManager(const std::string& path = "contacts.dat") : filePath(path) {
        Load();
    }

    void Load() {
        std::ifstream file(filePath);
        if (!file.is_open()) return;

        contacts.clear();
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            
            std::istringstream iss(line);
            Contact c;
            std::string lastConn, unattended;
            
            if (std::getline(iss, c.name, '|') &&
                std::getline(iss, c.ip, '|') &&
                iss >> c.port) {
                iss.ignore();
                std::getline(iss, c.deviceId, '|');
                iss >> c.lastConnected;
                iss.ignore();
                iss >> c.unattendedEnabled;
                contacts[c.name] = c;
            }
        }
    }

    void Save() {
        std::ofstream file(filePath);
        file << "# Remote Controller Contact List\n";
        file << "# Format: name|ip|port|deviceId|lastConnected|unattended\n";
        
        for (const auto& kv : contacts) {
            const Contact& c = kv.second;
            file << c.name << "|" << c.ip << "|" << c.port << "|"
                 << c.deviceId << "|" << c.lastConnected << "|"
                 << c.unattendedEnabled << "\n";
        }
    }

    void AddContact(const Contact& c) {
        contacts[c.name] = c;
        Save();
    }

    bool RemoveContact(const std::string& name) {
        if (contacts.erase(name) > 0) {
            Save();
            return true;
        }
        return false;
    }

    Contact* GetContact(const std::string& name) {
        auto it = contacts.find(name);
        return (it != contacts.end()) ? &it->second : nullptr;
    }

    std::vector<Contact> GetAllContacts() const {
        std::vector<Contact> result;
        for (const auto& kv : contacts) {
            result.push_back(kv.second);
        }
        return result;
    }

    void DisplayContacts() {
        if (contacts.empty()) {
            Console::PrintInfo("No saved contacts.");
            return;
        }

        Console::SetColor(Console::BRIGHT_WHITE);
        std::cout << "\n   +====+====================+==================+=======+\n";
        std::cout << "   | #  | Name               | IP Address       | Port  |\n";
        std::cout << "   +====+====================+==================+=======+\n";
        
        int idx = 1;
        for (const auto& kv : contacts) {
            const Contact& c = kv.second;
            std::cout << "   | " << std::setw(2) << idx++ << " | "
                      << std::setw(18) << std::left << c.name.substr(0, 18) << " | "
                      << std::setw(16) << std::left << c.ip << " | "
                      << std::setw(5) << c.port << " |\n";
        }
        std::cout << "   +====+====================+==================+=======+\n";
        Console::ResetColor();
    }

    Contact* GetContactByIndex(int index) {
        if (index < 1 || index > (int)contacts.size()) return nullptr;
        auto it = contacts.begin();
        std::advance(it, index - 1);
        return &it->second;
    }
};

// ============================================================================
// NETWORK HELPERS
// ============================================================================

class NetworkHelper {
public:
    static bool InitWinsock() {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }

    static void CleanupWinsock() {
        WSACleanup();
    }

    static SOCKET CreateTCPSocket() {
        return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    static bool SetSocketTimeout(SOCKET sock, int sendMs, int recvMs) {
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&sendMs, sizeof(sendMs));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&recvMs, sizeof(recvMs));
        return true;
    }

    static bool SetSocketNoDelay(SOCKET sock) {
        int flag = 1;
        return setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) == 0;
    }

    static bool SetSocketKeepalive(SOCKET sock) {
        // Enable TCP keepalive
        int keepalive = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(keepalive));

        // Set keepalive parameters (Windows-specific)
        struct tcp_keepalive {
            ULONG onoff;
            ULONG keepalivetime;
            ULONG keepaliveinterval;
        } alive;
        alive.onoff = 1;
        alive.keepalivetime = 5000;      // 5 seconds before first probe
        alive.keepaliveinterval = 1000;  // 1 second between probes
        
        DWORD bytesReturned;
        WSAIoctl(sock, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), nullptr, 0, &bytesReturned, nullptr, nullptr);
        
        return true;
    }

    static bool SetSocketBuffers(SOCKET sock, int sendBuf = 256 * 1024, int recvBuf = 256 * 1024) {
        // Larger buffers for better throughput
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sendBuf, sizeof(sendBuf));
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&recvBuf, sizeof(recvBuf));
        return true;
    }

    static bool SendAll(SOCKET sock, const void* data, size_t len) {
        const char* ptr = (const char*)data;
        size_t sent = 0;
        while (sent < len) {
            int r = send(sock, ptr + sent, (int)(len - sent), 0);
            if (r <= 0) return false;
            sent += r;
        }
        return true;
    }

    static bool RecvAll(SOCKET sock, void* data, size_t len) {
        char* ptr = (char*)data;
        size_t received = 0;
        while (received < len) {
            int r = recv(sock, ptr + received, (int)(len - received), 0);
            if (r <= 0) return false;
            received += r;
        }
        return true;
    }

    static bool SendPacket(SOCKET sock, uint8_t type, const void* payload, size_t payloadLen,
                          SecureChannel* crypto = nullptr, uint32_t seq = 0) {
        PacketHeader header;
        header.magic = RC_MAGIC;
        header.type = type;
        header.sequence = seq;

        if (crypto && crypto->IsReady() && payload && payloadLen > 0) {
            auto encrypted = crypto->Encrypt((const uint8_t*)payload, payloadLen);
            if (encrypted.empty()) return false;
            header.length = (uint32_t)encrypted.size();
            if (!SendAll(sock, &header, sizeof(header))) return false;
            if (!SendAll(sock, encrypted.data(), encrypted.size())) return false;
        } else {
            header.length = (uint32_t)payloadLen;
            if (!SendAll(sock, &header, sizeof(header))) return false;
            if (payloadLen > 0 && !SendAll(sock, payload, payloadLen)) return false;
        }
        return true;
    }

    static bool RecvPacket(SOCKET sock, PacketHeader& header, std::vector<uint8_t>& payload,
                          SecureChannel* crypto = nullptr) {
        if (!RecvAll(sock, &header, sizeof(header))) return false;
        if (header.magic != RC_MAGIC) return false;

        if (header.length > 0) {
            std::vector<uint8_t> raw(header.length);
            if (!RecvAll(sock, raw.data(), header.length)) return false;

            if (crypto && crypto->IsReady()) {
                payload = crypto->Decrypt(raw.data(), raw.size());
                if (payload.empty() && header.length > 0) return false;
            } else {
                payload = std::move(raw);
            }
        } else {
            payload.clear();
        }
        return true;
    }

    static std::string GetLocalIP() {
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        
        struct addrinfo hints = {}, *info;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        if (getaddrinfo(hostname, nullptr, &hints, &info) == 0) {
            const sockaddr_in* addr = (const sockaddr_in*)info->ai_addr;
            const char* ip = inet_ntoa(addr->sin_addr);
            freeaddrinfo(info);
            return ip ? ip : "127.0.0.1";
        }
        return "127.0.0.1";
    }
};

// ============================================================================
// DEVICE ID GENERATOR (from unique_generator.cpp)
// ============================================================================

#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "crypt32.lib")

class DeviceID {
public:
    static std::string Generate() {
        std::string combined;
        
        // Get volume serial
        DWORD serial = 0;
        GetVolumeInformationA("C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);
        combined += std::to_string(serial) + "|";

        // Get computer name
        char compName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = sizeof(compName);
        GetComputerNameA(compName, &size);
        combined += std::string(compName) + "|";

        // Get MAC address
        DWORD adapterSize = 0;
        GetAdaptersInfo(nullptr, &adapterSize);
        if (adapterSize > 0) {
            std::vector<BYTE> buf(adapterSize);
            PIP_ADAPTER_INFO info = (PIP_ADAPTER_INFO)buf.data();
            if (GetAdaptersInfo(info, &adapterSize) == ERROR_SUCCESS) {
                char mac[32];
                std::snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
                              info->Address[0], info->Address[1], info->Address[2],
                              info->Address[3], info->Address[4], info->Address[5]);
                combined += mac;
            }
        }

        // SHA-256 hash
        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        BYTE hash[32] = {0};
        DWORD hashLen = 32;
        bool hashReady = false;

        if (CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
                CryptHashData(hHash, (BYTE*)combined.c_str(), (DWORD)combined.length(), 0);
                hashReady = CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0) == TRUE;
                CryptDestroyHash(hHash);
            }
            CryptReleaseContext(hProv, 0);
        }

        // Convert to readable format: XXX-XXX-XXX
        uint64_t val = 0;
        if (hashReady && hashLen >= sizeof(uint64_t)) {
            memcpy(&val, hash, sizeof(uint64_t));
        } else {
            // Fallback keeps deterministic ID format even if crypto APIs fail.
            val = (uint64_t)std::hash<std::string>{}(combined);
        }
        val %= 1000000000ULL;

        std::ostringstream oss;
        oss << std::setw(3) << std::setfill('0') << ((val / 1000000) % 1000)
            << "-" << std::setw(3) << std::setfill('0') << ((val / 1000) % 1000)
            << "-" << std::setw(3) << std::setfill('0') << (val % 1000);

        return oss.str();
    }
};

// ============================================================================
// DPI AWARENESS
// ============================================================================

inline void EnableDPIAwareness() {
    HMODULE hUser32 = LoadLibraryA("user32.dll");
    if (!hUser32) return;

    // Try SetProcessDpiAwarenessContext (Windows 10 1703+)
    auto fnContext = (BOOL(WINAPI*)(void*))GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (fnContext) {
        fnContext((void*)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        FreeLibrary(hUser32);
        return;
    }

    // Fallback to SetProcessDPIAware
    auto fn = (BOOL(WINAPI*)())GetProcAddress(hUser32, "SetProcessDPIAware");
    if (fn) fn();
    FreeLibrary(hUser32);
}

inline std::string GetBaseFileName(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

inline std::string SanitizeFileName(const std::string& name) {
    std::string result;
    result.reserve(name.size());

    for (size_t i = 0; i < name.size(); i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 32 || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == ':' || ch == '*' || ch == '?' || ch == '\\' || ch == '/') {
            result.push_back('_');
        } else {
            result.push_back((char)ch);
        }
    }

    if (result.empty()) {
        result = "file.bin";
    }

    return result;
}

// ============================================================================
// LOW-SPEC OPTIMIZER: FAST FRAME DIFFERENCE DETECTOR
// ============================================================================

class FastFrameDiff {
public:
    static bool HasScreenChanged(const uint8_t* prevBuf, const uint8_t* currBuf, size_t size, uint32_t threshold = 100) {
        if (!prevBuf || !currBuf || size == 0) return true;
        
        size_t changedBytes = 0;
        size_t stride = 16;  // Sample every 16th byte (sub-millisecond low-CPU check)
        for (size_t i = 0; i < size; i += stride) {
            if (prevBuf[i] != currBuf[i]) {
                changedBytes++;
                if (changedBytes > threshold) return true;
            }
        }
        return false;
    }
};

// ============================================================================
// CLIPBOARD UTILITIES
// ============================================================================

class ClipboardHelper {
public:
    static std::string GetText() {
        if (!OpenClipboard(nullptr)) return "";
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (!hData) {
            hData = GetClipboardData(CF_UNICODETEXT);
            if (!hData) {
                CloseClipboard();
                return "";
            }
            wchar_t* wstr = (wchar_t*)GlobalLock(hData);
            if (!wstr) { CloseClipboard(); return ""; }
            int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
            std::string result(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
            GlobalUnlock(hData);
            CloseClipboard();
            if (!result.empty() && result.back() == '\0') result.pop_back();
            return result;
        }
        char* str = (char*)GlobalLock(hData);
        std::string result = str ? str : "";
        GlobalUnlock(hData);
        CloseClipboard();
        return result;
    }

    static bool SetText(const std::string& text) {
        if (text.empty()) return false;
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();
        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (!hGlob) { CloseClipboard(); return false; }
        char* ptr = (char*)GlobalLock(hGlob);
        if (ptr) {
            memcpy(ptr, text.c_str(), text.size() + 1);
            GlobalUnlock(hGlob);
            SetClipboardData(CF_TEXT, hGlob);
        }
        CloseClipboard();
        return true;
    }
};

