/*
 * Remote Controller - Background Service Monitor
 * Runs as a lightweight service that monitors for incoming connections
 * and launches the main agent when a connection request is detected.
 * 
 * Features:
 * - Minimal resource usage when idle
 * - Auto-starts agent on connection request
 * - Maintains allowed IP list
 * - Can run at Windows startup
 */

#include "rc_common.h"
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#pragma comment(lib, "Shell32.lib")

// ============================================================================
// SERVICE CONFIGURATION
// ============================================================================

struct ServiceConfig {
    uint16_t listenPort = RC_DEFAULT_PORT;
    bool unattendedMode = false;
    std::string agentPath;
    std::vector<std::string> allowedIPs;  // Empty = allow all
    bool autoStartWithWindows = false;
};

ServiceConfig g_serviceConfig;

// ============================================================================
// CONFIGURATION FILE MANAGEMENT
// ============================================================================

class ServiceConfigManager {
private:
    std::string configPath;

public:
    ServiceConfigManager() {
        // Store config in AppData
        char appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
            configPath = std::string(appData) + "\\RemoteController\\";
            CreateDirectoryA(configPath.c_str(), nullptr);
            configPath += "service_config.ini";
        } else {
            configPath = "service_config.ini";
        }
    }

    void Load(ServiceConfig& config) {
        std::ifstream file(configPath);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            while (!key.empty() && isspace(key.back())) key.pop_back();
            while (!value.empty() && isspace(value.front())) value.erase(0, 1);

            if (key == "port") {
                try { config.listenPort = (uint16_t)std::stoi(value); } catch (...) {}
            }
            else if (key == "unattended") {
                config.unattendedMode = (value == "1" || value == "true");
            }
            else if (key == "agent_path") {
                config.agentPath = value;
            }
            else if (key == "allowed_ip") {
                config.allowedIPs.push_back(value);
            }
            else if (key == "autostart") {
                config.autoStartWithWindows = (value == "1" || value == "true");
            }
        }
    }

    void Save(const ServiceConfig& config) {
        std::ofstream file(configPath);
        file << "# Remote Controller Service Configuration\n";
        file << "# Edit this file to change settings\n\n";
        file << "port=" << config.listenPort << "\n";
        file << "unattended=" << (config.unattendedMode ? "1" : "0") << "\n";
        file << "agent_path=" << config.agentPath << "\n";
        file << "autostart=" << (config.autoStartWithWindows ? "1" : "0") << "\n";
        file << "\n# Allowed IPs (one per line, empty = allow all)\n";
        for (const auto& ip : config.allowedIPs) {
            file << "allowed_ip=" << ip << "\n";
        }
    }

    std::string GetConfigPath() const { return configPath; }
};

// ============================================================================
// ALLOWED IPS MANAGEMENT
// ============================================================================

class AllowedIPManager {
private:
    std::string filePath;
    std::vector<std::string> allowedIPs;

public:
    AllowedIPManager() {
        char appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
            filePath = std::string(appData) + "\\RemoteController\\allowed_ips.txt";
        } else {
            filePath = "allowed_ips.txt";
        }
        Load();
    }

    void Load() {
        allowedIPs.clear();
        std::ifstream file(filePath);
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line[0] != '#') {
                allowedIPs.push_back(line);
            }
        }
    }

    void Save() {
        std::ofstream file(filePath);
        file << "# Allowed IP addresses for unattended access\n";
        file << "# One IP per line\n";
        for (const auto& ip : allowedIPs) {
            file << ip << "\n";
        }
    }

    bool IsAllowed(const std::string& ip) {
        if (allowedIPs.empty()) return true;  // Allow all if list is empty
        for (const auto& allowed : allowedIPs) {
            if (allowed == ip) return true;
        }
        return false;
    }

    void AddIP(const std::string& ip) {
        if (!IsAllowed(ip)) {
            allowedIPs.push_back(ip);
            Save();
        }
    }

    void RemoveIP(const std::string& ip) {
        allowedIPs.erase(
            std::remove(allowedIPs.begin(), allowedIPs.end(), ip),
            allowedIPs.end()
        );
        Save();
    }

    const std::vector<std::string>& GetAllowed() const { return allowedIPs; }

    void Display() {
        if (allowedIPs.empty()) {
            Console::PrintInfo("No IP restrictions (accepting all connections)");
            return;
        }
        Console::SetColor(Console::BRIGHT_WHITE);
        std::cout << "\n   Allowed IPs:\n";
        for (size_t i = 0; i < allowedIPs.size(); i++) {
            std::cout << "   [" << (i + 1) << "] " << allowedIPs[i] << "\n";
        }
        Console::ResetColor();
    }
};

// ============================================================================
// PROCESS MANAGEMENT
// ============================================================================

bool IsProcessRunning(const char* processName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    bool found = false;
    if (Process32First(hSnap, &pe)) {
        do {
            if (lstrcmpiA(pe.szExeFile, processName) == 0) {
                found = true;
                break;
            }
        } while (Process32Next(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return found;
}

bool LaunchAgent(const std::string& agentPath, const std::string& viewerIP) {
    // Build command line with viewer IP
    std::string cmdLine = "\"" + agentPath + "\" --unattended --viewer-ip " + viewerIP;

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(
        nullptr,
        (LPSTR)cmdLine.c_str(),
        nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE,
        nullptr, nullptr,
        &si, &pi
    )) {
        Console::PrintError("Failed to launch agent: " + std::to_string(GetLastError()));
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    Console::PrintSuccess("Agent launched successfully");
    return true;
}

// ============================================================================
// STARTUP MANAGEMENT
// ============================================================================

bool AddToStartup(const std::string& exePath) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    std::string value = "\"" + exePath + "\" --service";
    LONG status = RegSetValueExA(hKey, "RemoteControllerService", 0, REG_SZ,
                                 (BYTE*)value.c_str(), (DWORD)value.length() + 1);
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

bool RemoveFromStartup() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LONG status = RegDeleteValueA(hKey, "RemoteControllerService");
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

bool IsInStartup() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type;
    LONG status = RegQueryValueExA(hKey, "RemoteControllerService", nullptr, &type, nullptr, nullptr);
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

// ============================================================================
// SERVICE MONITOR
// ============================================================================

class ServiceMonitor {
private:
    SOCKET listenSocket = INVALID_SOCKET;
    bool running = false;
    AllowedIPManager ipManager;
    SecureChannel crypto;

public:
    bool Start(const ServiceConfig& config) {
        if (!NetworkHelper::InitWinsock()) {
            Console::PrintError("Failed to initialize Winsock");
            return false;
        }

        listenSocket = NetworkHelper::CreateTCPSocket();
        if (listenSocket == INVALID_SOCKET) {
            Console::PrintError("Failed to create socket");
            return false;
        }

        int opt = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config.listenPort);

        if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            Console::PrintError("Failed to bind to port " + std::to_string(config.listenPort));
            closesocket(listenSocket);
            return false;
        }

        if (listen(listenSocket, 5) == SOCKET_ERROR) {
            Console::PrintError("Failed to listen");
            closesocket(listenSocket);
            return false;
        }

        running = true;
        Console::PrintSuccess("Service monitor started on port " + std::to_string(config.listenPort));
        return true;
    }

    void Stop() {
        running = false;
        if (listenSocket != INVALID_SOCKET) {
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }
        NetworkHelper::CleanupWinsock();
    }

    void Run(const ServiceConfig& config) {
        Console::PrintInfo("Monitoring for incoming connections...");
        Console::PrintInfo("Press Ctrl+C to stop");

        while (running) {
            // Set timeout for accept to allow clean shutdown
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);

            timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            int ready = select(0, &readSet, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;

            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);

            if (clientSocket == INVALID_SOCKET) continue;

            const char* clientIP = inet_ntoa(clientAddr.sin_addr);
            if (!clientIP) clientIP = "0.0.0.0";

            Console::PrintInfo("Connection attempt from: " + std::string(clientIP));

            // Check if IP is allowed
            if (!ipManager.IsAllowed(clientIP)) {
                Console::PrintWarning("Connection rejected - IP not in allowed list");
                closesocket(clientSocket);
                continue;
            }

            // Perform handshake to verify it's a legitimate viewer
            if (!PerformHandshake(clientSocket)) {
                Console::PrintWarning("Invalid handshake - connection rejected");
                closesocket(clientSocket);
                continue;
            }

            // Launch agent if not already running
            if (!IsProcessRunning("rc_agent.exe")) {
                Console::PrintSuccess("Launching agent for viewer: " + std::string(clientIP));
                
                std::ofstream pendingFile("pending_connection.tmp");
                pendingFile << clientIP << "\n";
                pendingFile.close();

                closesocket(clientSocket);
                clientSocket = INVALID_SOCKET;
                crypto.Cleanup();

                // Close listener socket so agent can take over the port cleanly
                Stop();

                // Launch and wait for agent process
                std::string cmdLine = "\"" + config.agentPath + "\" --unattended --viewer-ip " + clientIP;
                STARTUPINFOA si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};

                if (CreateProcessA(nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    Console::PrintSuccess("Agent launched (PID " + std::to_string(pi.dwProcessId) + "). Monitoring process...");
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    CloseHandle(pi.hProcess);
                    Console::PrintInfo("Agent session ended. Resuming service listener...");
                } else {
                    Console::PrintError("Failed to launch agent process: " + std::to_string(GetLastError()));
                }

                // Restart service listener
                if (!Start(config)) {
                    Console::PrintError("Failed to restart service monitor after agent exit");
                    break;
                }
            } else {
                Console::PrintInfo("Agent already running");
                closesocket(clientSocket);
                crypto.Cleanup();
            }
        }
    }

    bool PerformHandshake(SOCKET sock) {
        if (!crypto.Initialize()) return false;

        NetworkHelper::SetSocketTimeout(sock, 5000, 5000);

        // Wait for handshake init
        PacketHeader header;
        std::vector<uint8_t> payload;

        if (!NetworkHelper::RecvPacket(sock, header, payload, nullptr)) {
            return false;
        }

        if (header.type != MSG_HANDSHAKE_INIT) {
            return false;
        }

        // Verify version compatibility
        if (payload.size() >= sizeof(HandshakeInit)) {
            HandshakeInit* init = (HandshakeInit*)payload.data();
            Console::PrintInfo("Viewer version: " + std::string(init->version));
        }

        return true;  // Basic handshake successful
    }

    AllowedIPManager& GetIPManager() { return ipManager; }
};

// ============================================================================
// INTERACTIVE CONFIGURATION
// ============================================================================

void ConfigureService(ServiceConfig& config, ServiceConfigManager& configMgr) {
    Console::ClearScreen();
    Console::SetColor(Console::BRIGHT_CYAN);
    std::cout << "\n   ===================================================\n";
    std::cout << "              SERVICE CONFIGURATION\n";
    std::cout << "   ===================================================\n\n";
    Console::ResetColor();

    // Port
    Console::PrintInfo("Current port: " + std::to_string(config.listenPort));
    std::string portStr = Console::GetInput("New port (Enter to keep current)");
    if (!portStr.empty()) {
        try { config.listenPort = (uint16_t)std::stoi(portStr); } catch (...) {}
    }

    // Agent path
    Console::PrintInfo("Current agent path: " + (config.agentPath.empty() ? "(not set)" : config.agentPath));
    std::string agentPath = Console::GetInput("Agent executable path (Enter to keep)");
    if (!agentPath.empty()) {
        config.agentPath = agentPath;
    }

    // Unattended mode
    Console::PrintInfo("Unattended mode: " + std::string(config.unattendedMode ? "ENABLED" : "DISABLED"));
    std::string unattended = Console::GetInput("Enable unattended mode? (y/n)");
    if (unattended == "y" || unattended == "Y") {
        config.unattendedMode = true;
    } else if (unattended == "n" || unattended == "N") {
        config.unattendedMode = false;
    }

    // Auto-start
    Console::PrintInfo("Auto-start with Windows: " + std::string(IsInStartup() ? "YES" : "NO"));
    std::string autostart = Console::GetInput("Enable auto-start? (y/n)");
    if (autostart == "y" || autostart == "Y") {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (AddToStartup(exePath)) {
            Console::PrintSuccess("Added to Windows startup");
            config.autoStartWithWindows = true;
        } else {
            Console::PrintError("Failed to add to startup");
        }
    } else if (autostart == "n" || autostart == "N") {
        RemoveFromStartup();
        Console::PrintInfo("Removed from Windows startup");
        config.autoStartWithWindows = false;
    }

    configMgr.Save(config);
    Console::PrintSuccess("Configuration saved!");
    Sleep(1500);
}

void ManageAllowedIPs(AllowedIPManager& ipManager) {
    while (true) {
        Console::ClearScreen();
        Console::SetColor(Console::BRIGHT_CYAN);
        std::cout << "\n   ===================================================\n";
        std::cout << "              ALLOWED IP MANAGEMENT\n";
        std::cout << "   ===================================================\n\n";
        Console::ResetColor();

        ipManager.Display();

        std::vector<std::string> options = {
            "Add IP Address",
            "Remove IP Address",
            "Clear All (Allow Everyone)",
            "Back"
        };

        int choice = Console::GetMenuChoice(options);

        switch (choice) {
            case 1: {
                std::string ip = Console::GetInput("Enter IP address to allow");
                if (!ip.empty()) {
                    ipManager.AddIP(ip);
                    Console::PrintSuccess("IP added!");
                }
                Sleep(1000);
                break;
            }
            case 2: {
                ipManager.Display();
                std::string idxStr = Console::GetInput("Enter number to remove");
                try {
                    int idx = std::stoi(idxStr) - 1;
                    const auto& allowed = ipManager.GetAllowed();
                    if (idx >= 0 && idx < (int)allowed.size()) {
                        ipManager.RemoveIP(allowed[idx]);
                        Console::PrintSuccess("IP removed!");
                    }
                } catch (...) {}
                Sleep(1000);
                break;
            }
            case 3: {
                std::string confirm = Console::GetInput("Clear all IPs? This will allow ANYONE to connect (y/n)");
                if (confirm == "y" || confirm == "Y") {
                    const auto& allowed = ipManager.GetAllowed();
                    while (!allowed.empty()) {
                        ipManager.RemoveIP(allowed[0]);
                    }
                    Console::PrintSuccess("All IP restrictions removed");
                }
                Sleep(1000);
                break;
            }
            case 4:
            default:
                return;
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    Console::InitConsole();
    Console::SetTitle("Remote Controller - Service Monitor");

    ServiceConfigManager configMgr;
    ServiceConfig config;
    configMgr.Load(config);

    // If no agent path set, use default
    if (config.agentPath.empty()) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir = exePath;
        size_t lastSlash = dir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            dir = dir.substr(0, lastSlash + 1);
        }
        config.agentPath = dir + "rc_agent.exe";
    }

    // Check for command line arguments
    bool serviceMode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--service") == 0 || strcmp(argv[i], "-s") == 0) {
            serviceMode = true;
        }
    }

    // Service mode - run silently in background
    if (serviceMode) {
        // Hide console window
        HWND hConsole = GetConsoleWindow();
        if (hConsole) ShowWindow(hConsole, SW_HIDE);

        ServiceMonitor monitor;
        if (monitor.Start(config)) {
            monitor.Run(config);
        }
        return 0;
    }

    // Interactive mode
    while (true) {
        Console::ClearScreen();
        Console::PrintLogo();

        Console::SetColor(Console::BRIGHT_MAGENTA);
        std::cout << "                      [ SERVICE MONITOR ]\n\n";
        Console::ResetColor();

        Console::PrintInfo("Version: " RC_VERSION);
        Console::PrintInfo("Device ID: " + DeviceID::Generate());
        Console::PrintInfo("Status: " + std::string(IsInStartup() ? "Auto-start ENABLED" : "Auto-start DISABLED"));

        std::vector<std::string> options = {
            "Start Service Monitor",
            "Configure Service",
            "Manage Allowed IPs",
            "Run in Background",
            "Exit"
        };

        int choice = Console::GetMenuChoice(options);

        switch (choice) {
            case 1: {
                Console::ClearScreen();
                Console::PrintLogo();
                
                ServiceMonitor monitor;
                if (monitor.Start(config)) {
                    monitor.Run(config);
                }
                break;
            }
            case 2:
                ConfigureService(config, configMgr);
                break;
            case 3: {
                ServiceMonitor monitor;
                ManageAllowedIPs(monitor.GetIPManager());
                break;
            }
            case 4: {
                // Launch self in service mode
                char exePath[MAX_PATH];
                GetModuleFileNameA(nullptr, exePath, MAX_PATH);
                
                std::string cmdLine = "\"" + std::string(exePath) + "\" --service";
                
                STARTUPINFOA si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};

                if (CreateProcessA(nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    Console::PrintSuccess("Service started in background!");
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                } else {
                    Console::PrintError("Failed to start background service");
                }
                Sleep(1500);
                break;
            }
            case 5:
            default:
                return 0;
        }
    }

    return 0;
}

