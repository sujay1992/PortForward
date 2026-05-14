# Multithreaded TCP Port Forwarder

A lightweight, multithreaded TCP port forwarder written in C++ with both Windows and Linux implementations. Supports multiple forwarding rules, each mapping a local listening port to a remote host:port. Configuration is read from `pf_settings.txt`.

## Files

| File | Description |
|------|-------------|
| `PortForward.cpp` | Windows implementation (Winsock2, Win32 threads) |
| `port_forward_linux.cpp` | Linux implementation (POSIX sockets, pthreads) |
| `settings.txt` | Configuration file (shared format for both platforms) |

## Features

- **Protocol-agnostic** — Pure TCP relay. Works for HTTP, HTTPS, SSH, RDP, databases, or any TCP-based protocol.
- **Multiple forwarding rules** — Define as many listen:port → remote:port mappings as needed.
- **One listener thread per rule** — Each forwarding rule gets its own dedicated listener thread.
- **One handler thread per connection** — Concurrent connections are handled in parallel.
- **Full-duplex bidirectional relay** — Data flows both directions simultaneously via `select()`.
- **Complete send loops** — Handles partial writes correctly.
- **Configurable** — All settings read from `settings.txt` (or custom path via command-line argument).
- **Thread limiting** — Rejects new connections when the global thread limit is reached.
- **Timestamped logging** — Logs to stdout and optionally to a log file. Thread-safe.
- **Connection timeouts** — Non-blocking connect with configurable timeout for remote connections.
- **DNS resolution** — Remote hosts can be hostnames or IP addresses.

## Architecture

```
   Client A ──► :9080 ─── Listener Thread 1 ──► 192.168.56.101:80
                              │
                         Handler Thread ──► relay(client, remote)
                              │
   Client B ──► :9443 ─── Listener Thread 2 ──► 192.168.56.101:443
                              │
                         Handler Thread ──► relay(client, remote)
                              │
   Client C ──► :2222 ─── Listener Thread 3 ──► 192.168.56.101:22
                              │
                         Handler Thread ──► relay(client, remote)
```

### Connection Flow

1. A listener thread accepts an incoming connection on a local port.
2. A new handler thread is spawned for the connection.
3. The handler connects to the configured remote host:port.
4. The handler enters a bidirectional relay loop using `select()`.
5. All bytes received from the client are forwarded to the remote, and vice versa.
6. When either side disconnects, both sockets are closed and the thread exits.

## Configuration

### settings.txt

```ini
# Port Forwarder Settings
# Lines starting with '#' are comments

# Global settings
max_threads=200
buffer_size=65536
connect_timeout=10
recv_timeout=60
logging=1
log_file=port_forward.log

# Forwarding rules (one per line):
#   listen_address:listen_port -> remote_host:remote_port
#
# Examples:
#   0.0.0.0:8080 -> 192.168.1.100:80
#   127.0.0.1:3389 -> 10.0.0.5:3389
#   0.0.0.0:2222 -> myserver.example.com:22

[rules]
0.0.0.0:9080 -> 192.168.56.101:80
0.0.0.0:9443 -> 192.168.56.101:443
0.0.0.0:2222 -> 192.168.56.101:22
```

### Global Settings Reference

| Setting | Default | Description |
|---------|---------|-------------|
| `max_threads` | `200` | Maximum total concurrent handler threads across all rules. Excess connections are rejected. |
| `buffer_size` | `65536` | Read/write buffer size in bytes for socket I/O |
| `connect_timeout` | `10` | Timeout in seconds for connecting to remote hosts |
| `recv_timeout` | `60` | Receive/send timeout in seconds on all sockets and the `select()` idle timeout |
| `logging` | `1` | Enable (`1`) or disable (`0`) logging |
| `log_file` | *(empty)* | Path to log file. If empty, logs go to stdout only. |

### Forwarding Rules Format

Rules are defined under the `[rules]` section, one per line:

```
listen_address:listen_port -> remote_host:remote_port
```

| Component | Description | Examples |
|-----------|-------------|----------|
| `listen_address` | Local bind address | `0.0.0.0` (all interfaces), `127.0.0.1` (localhost only) |
| `listen_port` | Local port to listen on | `9080`, `2222`, `3389` |
| `remote_host` | Target host (IP or hostname) | `192.168.56.101`, `myserver.example.com` |
| `remote_port` | Target port on remote host | `80`, `22`, `443` |

## Building

### Windows (MSVC)

```cmd
cl /EHsc /O2 PortForward.cpp /link ws2_32.lib
```

### Windows (MinGW g++)

```cmd
g++ -O2 -o PortForward.exe PortForward.cpp -lws2_32
```

### Linux (g++)

```bash
g++ -std=c++11 -O2 -pthread -o port_forward port_forward_linux.cpp
```

**Minimum C++ standard:** C++11

## Running

### Windows

```cmd
PortForward.exe                       # uses settings.txt in current directory
PortForward.exe myconfig.txt          # custom settings file
```

### Linux

```bash
./port_forward                         # uses settings.txt in current directory
./port_forward myconfig.txt            # custom settings file
```

## Use Cases

### Forward HTTP traffic to a remote web server

```ini
[rules]
0.0.0.0:8080 -> 192.168.1.100:80
```
Browse to `http://localhost:8080` → traffic goes to `192.168.1.100:80`.

### Expose an SSH server on a different port

```ini
[rules]
0.0.0.0:2222 -> 10.0.0.5:22
```
`ssh -p 2222 localhost` → connects to `10.0.0.5:22`.

### Forward RDP to a remote machine

```ini
[rules]
127.0.0.1:3389 -> 10.0.0.10:3389
```
RDP to `localhost` → connects to `10.0.0.10:3389`.

### Multiple rules simultaneously

```ini
[rules]
0.0.0.0:9080 -> webserver.internal:80
0.0.0.0:9443 -> webserver.internal:443
0.0.0.0:5433 -> dbserver.internal:5432
0.0.0.0:2222 -> jumpbox.internal:22
```

## Platform Differences

| Aspect | Windows (`PortForward.cpp`) | Linux (`port_forward_linux.cpp`) |
|--------|------------------------------|----------------------------------|
| Sockets | Winsock2 (`SOCKET`, `closesocket`, `SD_BOTH`) | POSIX (`int` fd, `close`, `SHUT_RDWR`) |
| Threading | `CreateThread` / `CloseHandle` | `pthread_create` with `PTHREAD_CREATE_DETACHED` |
| Thread count | `InterlockedIncrement` / `InterlockedDecrement` | `std::atomic<int>` |
| Log mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| Non-blocking | `ioctlsocket(FIONBIO)` | `fcntl(O_NONBLOCK)` |
| Socket timeout | `DWORD` milliseconds | `struct timeval` seconds |
| select() nfds | `0` (ignored on Windows) | `max_fd + 1` (required on Linux) |
| Log file open | `_fsopen(..., _SH_DENYNO)` (shared read access) | `fopen()` (no locking issues on Linux) |
| Log file flush | `fflush` + `_commit` (forces metadata update) | `fflush` (sufficient on Linux) |
| Signal handling | N/A | `signal(SIGPIPE, SIG_IGN)` |
| Wait for listeners | `WaitForMultipleObjects` | `pthread_join` |
| Initialization | `WSAStartup` / `WSACleanup` | None needed |

## Log Output

Logs are timestamped and include the listen port, client IP:port, and remote target:

```
[2026-05-14 10:00:01] === Port Forwarder Starting ===
[2026-05-14 10:00:01] Rules: 3  MaxThreads: 200  Buffer: 65536
[2026-05-14 10:00:01] [:9080] Listening on 0.0.0.0:9080 -> 192.168.56.101:80
[2026-05-14 10:00:01] [:9443] Listening on 0.0.0.0:9443 -> 192.168.56.101:443
[2026-05-14 10:00:01] [:2222] Listening on 0.0.0.0:2222 -> 192.168.56.101:22
[2026-05-14 10:00:01] All 3 listeners started. Press Ctrl+C to stop.
[2026-05-14 10:00:05] [:9080] 192.168.1.10:52341 connected, forwarding to 192.168.56.101:80
[2026-05-14 10:00:05] [:9080] Tunnel established: 192.168.1.10:52341 <-> 192.168.56.101:80
[2026-05-14 10:00:08] [:9080] 192.168.1.10:52341 disconnected
```

## Comparison with Proxy Server

| Feature | Port Forwarder | Proxy Server |
|---------|---------------|--------------|
| Protocol awareness | None (raw TCP) | HTTP/HTTPS |
| Target destination | Fixed per rule | Dynamic (from request URL) |
| Client configuration | None needed | Browser proxy settings required |
| HTTPS handling | Transparent pass-through | CONNECT tunnel |
| Use case | Expose specific services | General web browsing proxy |
| Multiple targets | Multiple rules, multiple ports | Single port, any target |
| Request rewriting | None | URL rewriting (absolute → relative) |

## Firewall Notes

Ensure the firewall allows inbound traffic on all configured listen ports:

```bash
# Linux: firewalld
sudo firewall-cmd --add-port=9080/tcp --add-port=9443/tcp --add-port=2222/tcp --permanent
sudo firewall-cmd --reload

# Linux: iptables
sudo iptables -I INPUT -p tcp --dport 9080 -j ACCEPT
sudo iptables -I INPUT -p tcp --dport 9443 -j ACCEPT
sudo iptables -I INPUT -p tcp --dport 2222 -j ACCEPT

# Linux: ufw
sudo ufw allow 9080/tcp
sudo ufw allow 9443/tcp
sudo ufw allow 2222/tcp
```

On Windows, the firewall prompt usually appears automatically on first run. If not:

```cmd
netsh advfirewall firewall add rule name="Port Forwarder" dir=in action=allow protocol=tcp localport=9080,9443,2222
```
