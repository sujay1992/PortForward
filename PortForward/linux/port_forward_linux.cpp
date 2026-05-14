/*
 * Multithreaded TCP Port Forwarder for Linux
 * Reads configuration from settings.txt
 * Build: g++ -std=c++11 -O2 -pthread -o port_forward port_forward_linux.cpp
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <atomic>

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
    int         max_threads     = 200;
    int         buffer_size     = 65536;
    int         connect_timeout = 10;
    int         recv_timeout    = 60;
    bool        logging         = true;
    std::string log_file;
    std::vector<ForwardRule> rules;
};

static Settings          g_settings;
static std::atomic<int>  g_active_threads(0);
static FILE*             g_log_fp = nullptr;
static pthread_mutex_t   g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log_msg(const char* fmt, ...) {
    if (!g_settings.logging) return;

    pthread_mutex_lock(&g_log_mutex);

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

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
    }

    va_end(ap);
    pthread_mutex_unlock(&g_log_mutex);
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
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if      (key == "max_threads")     g_settings.max_threads     = atoi(val.c_str());
            else if (key == "buffer_size")     g_settings.buffer_size     = atoi(val.c_str());
            else if (key == "connect_timeout") g_settings.connect_timeout = atoi(val.c_str());
            else if (key == "recv_timeout")    g_settings.recv_timeout    = atoi(val.c_str());
            else if (key == "logging")         g_settings.logging         = (val == "1");
            else if (key == "log_file")        g_settings.log_file        = val;
        } else {
            size_t arrow = line.find("->");
            if (arrow == std::string::npos) continue;

            std::string left  = trim(line.substr(0, arrow));
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
static void close_socket(int& s) {
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
        s = -1;
    }
}

// ---------------------------------------------------------------------------
// Set socket timeouts
// ---------------------------------------------------------------------------
static void set_socket_timeouts(int s, int recv_sec, int send_sec) {
    struct timeval recv_tv = { recv_sec, 0 };
    struct timeval send_tv = { send_sec, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));
}

// ---------------------------------------------------------------------------
// Connect to remote host with timeout
// ---------------------------------------------------------------------------
static int connect_to_host(const char* host, int port) {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_msg("DNS resolution failed for %s", host);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // Non-blocking connect with timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0) {
        if (errno != EINPROGRESS) {
            close_socket(sock);
            return -1;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec  = g_settings.connect_timeout;
        tv.tv_usec = 0;

        ret = select(sock + 1, nullptr, &wset, nullptr, &tv);
        if (ret <= 0) {
            log_msg("Connection to %s:%d timed out", host, port);
            close_socket(sock);
            return -1;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            log_msg("Connection to %s:%d failed (error %d)", host, port, so_error);
            close_socket(sock);
            return -1;
        }
    }

    // Back to blocking
    fcntl(sock, F_SETFL, flags);

    set_socket_timeouts(sock, g_settings.recv_timeout, g_settings.recv_timeout);
    return sock;
}

// ---------------------------------------------------------------------------
// Bidirectional relay between two sockets
// ---------------------------------------------------------------------------
static void relay(int a, int b) {
    char* buf = new char[g_settings.buffer_size];
    int maxfd = (a > b ? a : b) + 1;
    fd_set rset;

    while (true) {
        FD_ZERO(&rset);
        FD_SET(a, &rset);
        FD_SET(b, &rset);

        struct timeval tv;
        tv.tv_sec  = g_settings.recv_timeout;
        tv.tv_usec = 0;

        int ret = select(maxfd, &rset, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        if (FD_ISSET(a, &rset)) {
            ssize_t n = recv(a, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = send(b, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }

        if (FD_ISSET(b, &rset)) {
            ssize_t n = recv(b, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = send(a, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }
    }

done:
    delete[] buf;
}

// ---------------------------------------------------------------------------
// Client handler thread
// ---------------------------------------------------------------------------
struct ClientContext {
    int         client_socket;
    sockaddr_in client_addr;
    std::string remote_host;
    int         remote_port;
    int         listen_port;
};

static void* client_thread(void* param) {
    g_active_threads++;

    ClientContext* ctx = (ClientContext*)param;
    int client = ctx->client_socket;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port   = ntohs(ctx->client_addr.sin_port);
    std::string rhost = ctx->remote_host;
    int rport         = ctx->remote_port;
    int lport         = ctx->listen_port;
    delete ctx;

    set_socket_timeouts(client, g_settings.recv_timeout, g_settings.recv_timeout);

    log_msg("[:%d] %s:%d connected, forwarding to %s:%d",
            lport, client_ip, client_port, rhost.c_str(), rport);

    int remote = connect_to_host(rhost.c_str(), rport);
    if (remote < 0) {
        log_msg("[:%d] Cannot connect to %s:%d", lport, rhost.c_str(), rport);
        close_socket(client);
        g_active_threads--;
        return nullptr;
    }

    log_msg("[:%d] Tunnel established: %s:%d <-> %s:%d",
            lport, client_ip, client_port, rhost.c_str(), rport);

    relay(client, remote);

    log_msg("[:%d] %s:%d disconnected", lport, client_ip, client_port);

    close_socket(remote);
    close_socket(client);
    g_active_threads--;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Listener thread — one per forwarding rule
// ---------------------------------------------------------------------------
struct ListenerContext {
    ForwardRule rule;
};

static void* listener_thread(void* param) {
    ListenerContext* lctx = (ListenerContext*)param;
    ForwardRule rule = lctx->rule;
    delete lctx;

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        log_msg("[:%d] socket() failed: %s", rule.listen_port, strerror(errno));
        return (void*)1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)rule.listen_port);
    inet_pton(AF_INET, rule.listen_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("[:%d] bind() failed: %s", rule.listen_port, strerror(errno));
        close(listen_sock);
        return (void*)1;
    }

    if (listen(listen_sock, SOMAXCONN) < 0) {
        log_msg("[:%d] listen() failed: %s", rule.listen_port, strerror(errno));
        close(listen_sock);
        return (void*)1;
    }

    log_msg("[:%d] Listening on %s:%d -> %s:%d",
            rule.listen_port,
            rule.listen_address.c_str(), rule.listen_port,
            rule.remote_host.c_str(), rule.remote_port);

    while (true) {
        sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            log_msg("[:%d] accept() failed: %s", rule.listen_port, strerror(errno));
            continue;
        }

        if (g_active_threads >= g_settings.max_threads) {
            log_msg("[:%d] Thread limit reached (%d), rejecting", rule.listen_port, g_settings.max_threads);
            close(client_sock);
            continue;
        }

        ClientContext* ctx = new ClientContext;
        ctx->client_socket = client_sock;
        ctx->client_addr   = client_addr;
        ctx->remote_host   = rule.remote_host;
        ctx->remote_port   = rule.remote_port;
        ctx->listen_port   = rule.listen_port;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, client_thread, ctx) != 0) {
            log_msg("[:%d] pthread_create failed: %s", rule.listen_port, strerror(errno));
            delete ctx;
            close(client_sock);
        }

        pthread_attr_destroy(&attr);
    }

    close(listen_sock);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* settings_path = "settings.txt";
    if (argc > 1) settings_path = argv[1];

    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    if (!load_settings(settings_path)) {
        fprintf(stderr, "No valid forwarding rules found in %s\n", settings_path);
        return 1;
    }

    // Open log file
    if (!g_settings.log_file.empty()) {
        g_log_fp = fopen(g_settings.log_file.c_str(), "a");
        if (!g_log_fp) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", g_settings.log_file.c_str());
        }
    }

    log_msg("=== Port Forwarder Starting ===");
    log_msg("Rules: %d  MaxThreads: %d  Buffer: %d",
            (int)g_settings.rules.size(), g_settings.max_threads, g_settings.buffer_size);

    // Launch one listener thread per forwarding rule
    std::vector<pthread_t> listener_tids;

    for (size_t i = 0; i < g_settings.rules.size(); i++) {
        ListenerContext* lctx = new ListenerContext;
        lctx->rule = g_settings.rules[i];

        pthread_t tid;
        if (pthread_create(&tid, nullptr, listener_thread, lctx) == 0) {
            listener_tids.push_back(tid);
        } else {
            log_msg("Failed to start listener for rule %d: %s", (int)i + 1, strerror(errno));
            delete lctx;
        }
    }

    if (listener_tids.empty()) {
        fprintf(stderr, "No listeners started. Exiting.\n");
        return 1;
    }

    log_msg("All %d listeners started. Press Ctrl+C to stop.", (int)listener_tids.size());

    // Wait for all listener threads (they run forever)
    for (pthread_t tid : listener_tids) {
        pthread_join(tid, nullptr);
    }

    // Cleanup (unreachable in normal operation)
    if (g_log_fp) fclose(g_log_fp);
    pthread_mutex_destroy(&g_log_mutex);
    return 0;
}
