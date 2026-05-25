/*
 * Multithreaded TCP Port Forwarder for Windows
 * Reads configuration from settings.txt
 * Build: cl /EHsc /O2 port_forward.cpp /link ws2_32.lib
 *    or: g++ -O2 -o port_forward.exe port_forward.cpp -lws2_32
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <share.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

 // ---------------------------------------------------------------------------
 // Forwarding rule
 // ---------------------------------------------------------------------------
struct ForwardRule {
    std::string listen_address;
    int         listen_port;
    std::string remote_host;
    int         remote_port;
};

// ---------------------------------------------------------------------------
// Global settings
// ---------------------------------------------------------------------------
struct Settings {
    int         max_threads = 200;
    int         buffer_size = 65536;
    int         connect_timeout = 10;
    int         recv_timeout = 60;
    bool        logging = true;
    std::string log_file;
    std::vector<ForwardRule> rules;
};

static Settings         g_settings;
static LONG             g_active_threads = 0;
static FILE* g_log_fp = nullptr;
static CRITICAL_SECTION g_log_cs;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log_msg(const char* fmt, ...) {
    if (!g_settings.logging) return;

    EnterCriticalSection(&g_log_cs);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char timestamp[64];
    _snprintf_s(timestamp, sizeof(timestamp), _TRUNCATE,
        "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[%s] ", timestamp);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);

    if (g_log_fp) {
        va_end(ap);
        va_start(ap, fmt);
        fprintf(g_log_fp, "[%s] ", timestamp);
        vfprintf(g_log_fp, fmt, ap);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
        _commit(_fileno(g_log_fp));
    }

    va_end(ap);
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------------------------
// Trim helper
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Parse "address:port" string
// ---------------------------------------------------------------------------
static bool parse_endpoint(const std::string& s, std::string& host, int& port) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = trim(s.substr(0, colon));
    port = atoi(trim(s.substr(colon + 1)).c_str());
    return port > 0 && port <= 65535;
}

// ---------------------------------------------------------------------------
// Load settings from file
// ---------------------------------------------------------------------------
static bool load_settings(const char* path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        fprintf(stderr, "Error: Cannot open %s\n", path);
        return false;
    }

    bool in_rules = false;
    std::string line;

    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line == "[rules]") {
            in_rules = true;
            continue;
        }

        if (!in_rules) {
            // Global setting: key=value
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (key == "max_threads")     g_settings.max_threads = atoi(val.c_str());
            else if (key == "buffer_size")     g_settings.buffer_size = atoi(val.c_str());
            else if (key == "connect_timeout") g_settings.connect_timeout = atoi(val.c_str());
            else if (key == "recv_timeout")    g_settings.recv_timeout = atoi(val.c_str());
            else if (key == "logging")         g_settings.logging = (val == "1");
            else if (key == "log_file")        g_settings.log_file = val;
        }
        else {
            // Rule: listen_address:listen_port -> remote_host:remote_port
            size_t arrow = line.find("->");
            if (arrow == std::string::npos) continue;

            std::string left = trim(line.substr(0, arrow));
            std::string right = trim(line.substr(arrow + 2));

            ForwardRule rule;
            if (!parse_endpoint(left, rule.listen_address, rule.listen_port)) {
                fprintf(stderr, "Warning: Invalid listen endpoint: %s\n", left.c_str());
                continue;
            }
            if (!parse_endpoint(right, rule.remote_host, rule.remote_port)) {
                fprintf(stderr, "Warning: Invalid remote endpoint: %s\n", right.c_str());
                continue;
            }

            g_settings.rules.push_back(rule);
        }
    }

    return !g_settings.rules.empty();
}

// ---------------------------------------------------------------------------
// Close a socket safely
// ---------------------------------------------------------------------------
static void close_socket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

// ---------------------------------------------------------------------------
// Set socket timeouts
// ---------------------------------------------------------------------------
static void set_socket_timeouts(SOCKET s, int recv_sec, int send_sec) {
    DWORD recv_ms = recv_sec * 1000;
    DWORD send_ms = send_sec * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_ms, sizeof(recv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_ms, sizeof(send_ms));
}

// ---------------------------------------------------------------------------
// Connect to remote host with timeout
// ---------------------------------------------------------------------------
static SOCKET connect_to_host(const char* host, int port) {
    struct addrinfo hints = {}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    _snprintf_s(port_str, sizeof(port_str), _TRUNCATE, "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_msg("DNS resolution failed for %s", host);
        return INVALID_SOCKET;
    }

    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }

    // Non-blocking connect with timeout
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int ret = connect(sock, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            close_socket(sock);
            return INVALID_SOCKET;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec = g_settings.connect_timeout;
        tv.tv_usec = 0;

        ret = select(0, nullptr, &wset, nullptr, &tv);
        if (ret <= 0) {
            log_msg("Connection to %s:%d timed out", host, port);
            close_socket(sock);
            return INVALID_SOCKET;
        }

        int so_error = 0;
        int len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            log_msg("Connection to %s:%d failed (error %d)", host, port, so_error);
            close_socket(sock);
            return INVALID_SOCKET;
        }
    }

    // Back to blocking
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);

    set_socket_timeouts(sock, g_settings.recv_timeout, g_settings.recv_timeout);
    return sock;
}

// ---------------------------------------------------------------------------
// Bidirectional relay between two sockets
// ---------------------------------------------------------------------------
static void relay(SOCKET a, SOCKET b) {
    char* buf = new char[g_settings.buffer_size];
    fd_set rset;

    while (true) {
        FD_ZERO(&rset);
        FD_SET(a, &rset);
        FD_SET(b, &rset);

        struct timeval tv;
        tv.tv_sec = g_settings.recv_timeout;
        tv.tv_usec = 0;

        int ret = select(0, &rset, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        if (FD_ISSET(a, &rset)) {
            int n = recv(a, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            int sent = 0;
            while (sent < n) {
                int s = send(b, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }

        if (FD_ISSET(b, &rset)) {
            int n = recv(b, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            int sent = 0;
            while (sent < n) {
                int s = send(a, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }
    }

done:
    delete[] buf;
}

// ---------------------------------------------------------------------------
// Client handler thread — connects to remote and relays traffic
// ---------------------------------------------------------------------------
struct ClientContext {
    SOCKET      client_socket;
    sockaddr_in client_addr;
    std::string remote_host;
    int         remote_port;
    int         listen_port;  // for logging
};

static DWORD WINAPI client_thread(LPVOID param) {
    InterlockedIncrement(&g_active_threads);

    ClientContext* ctx = (ClientContext*)param;
    SOCKET client = ctx->client_socket;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(ctx->client_addr.sin_port);
    std::string rhost = ctx->remote_host;
    int rport = ctx->remote_port;
    int lport = ctx->listen_port;
    delete ctx;

    set_socket_timeouts(client, g_settings.recv_timeout, g_settings.recv_timeout);

    log_msg("[:%d] %s:%d connected, forwarding to %s:%d",
        lport, client_ip, client_port, rhost.c_str(), rport);

    SOCKET remote = connect_to_host(rhost.c_str(), rport);
    if (remote == INVALID_SOCKET) {
        log_msg("[:%d] Cannot connect to %s:%d", lport, rhost.c_str(), rport);
        close_socket(client);
        InterlockedDecrement(&g_active_threads);
        return 0;
    }

    log_msg("[:%d] Tunnel established: %s:%d <-> %s:%d",
        lport, client_ip, client_port, rhost.c_str(), rport);

    relay(client, remote);

    log_msg("[:%d] %s:%d disconnected", lport, client_ip, client_port);

    close_socket(remote);
    close_socket(client);
    InterlockedDecrement(&g_active_threads);
    return 0;
}

// ---------------------------------------------------------------------------
// Listener thread — one per forwarding rule
// ---------------------------------------------------------------------------
struct ListenerContext {
    ForwardRule rule;
};

static DWORD WINAPI listener_thread(LPVOID param) {
    ListenerContext* lctx = (ListenerContext*)param;
    ForwardRule rule = lctx->rule;
    delete lctx;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        log_msg("[:%d] socket() failed: %d", rule.listen_port, WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)rule.listen_port);
    inet_pton(AF_INET, rule.listen_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg("[:%d] bind() failed: %d", rule.listen_port, WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        log_msg("[:%d] listen() failed: %d", rule.listen_port, WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    log_msg("[:%d] Listening on %s:%d -> %s:%d",
        rule.listen_port,
        rule.listen_address.c_str(), rule.listen_port,
        rule.remote_host.c_str(), rule.remote_port);

    while (true) {
        sockaddr_in client_addr = {};
        int addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET) {
            log_msg("[:%d] accept() failed: %d", rule.listen_port, WSAGetLastError());
            continue;
        }

        if (g_active_threads >= g_settings.max_threads) {
            log_msg("[:%d] Thread limit reached (%d), rejecting", rule.listen_port, g_settings.max_threads);
            closesocket(client_sock);
            continue;
        }

        ClientContext* ctx = new ClientContext;
        ctx->client_socket = client_sock;
        ctx->client_addr = client_addr;
        ctx->remote_host = rule.remote_host;
        ctx->remote_port = rule.remote_port;
        ctx->listen_port = rule.listen_port;

        HANDLE hThread = CreateThread(nullptr, 0, client_thread, ctx, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        }
        else {
            log_msg("[:%d] CreateThread failed: %d", rule.listen_port, GetLastError());
            delete ctx;
            closesocket(client_sock);
        }
    }

    closesocket(listen_sock);
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* settings_path = "settings.txt";
    if (argc > 1) settings_path = argv[1];

    InitializeCriticalSection(&g_log_cs);

    if (!load_settings(settings_path)) {
        fprintf(stderr, "No valid forwarding rules found in %s\n", settings_path);
        return 1;
    }

    // Open log file
    if (!g_settings.log_file.empty()) {
        g_log_fp = _fsopen(g_settings.log_file.c_str(), "a", _SH_DENYNO);
        if (!g_log_fp) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", g_settings.log_file.c_str());
        }
    }

    log_msg("=== Port Forwarder Starting ===");
    log_msg("Rules: %d  MaxThreads: %d  Buffer: %d",
        (int)g_settings.rules.size(), g_settings.max_threads, g_settings.buffer_size);

    // Initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    // Launch one listener thread per forwarding rule
    std::vector<HANDLE> listener_handles;

    for (size_t i = 0; i < g_settings.rules.size(); i++) {
        ListenerContext* lctx = new ListenerContext;
        lctx->rule = g_settings.rules[i];

        HANDLE h = CreateThread(nullptr, 0, listener_thread, lctx, 0, nullptr);
        if (h) {
            listener_handles.push_back(h);
        }
        else {
            log_msg("Failed to start listener for rule %d: %d", (int)i + 1, GetLastError());
            delete lctx;
        }
    }

    if (listener_handles.empty()) {
        fprintf(stderr, "No listeners started. Exiting.\n");
        WSACleanup();
        return 1;
    }

    log_msg("All %d listeners started. Press Ctrl+C to stop.", (int)listener_handles.size());

    // Wait for all listener threads (they run forever)
    WaitForMultipleObjects((DWORD)listener_handles.size(),
        listener_handles.data(), TRUE, INFINITE);

    // Cleanup (unreachable in normal operation)
    for (HANDLE h : listener_handles) CloseHandle(h);
    WSACleanup();
    if (g_log_fp) fclose(g_log_fp);
    DeleteCriticalSection(&g_log_cs);
    return 0;
}
