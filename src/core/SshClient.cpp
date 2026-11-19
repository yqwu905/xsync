#include "core/SshClient.h"

#include "core/ShellUtils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace xsync {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr int64_t kInvalidSocket = -1;
inline NativeSocket toNative(int64_t fd) { return static_cast<NativeSocket>(fd); }
inline void closeSocket(int64_t& fd) {
    if (fd != kInvalidSocket) {
        ::closesocket(toNative(fd));
        fd = kInvalidSocket;
    }
}
inline bool setNonBlocking(int64_t fd, bool nb) {
    u_long mode = nb ? 1 : 0;
    return ::ioctlsocket(toNative(fd), FIONBIO, &mode) == 0;
}
inline bool lastSocketWouldBlock() {
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
#else
using NativeSocket = int;
constexpr int64_t kInvalidSocket = -1;
inline NativeSocket toNative(int64_t fd) { return static_cast<NativeSocket>(fd); }
inline void closeSocket(int64_t& fd) {
    if (fd != kInvalidSocket) {
        ::close(toNative(fd));
        fd = kInvalidSocket;
    }
}
inline bool setNonBlocking(int64_t fd, bool nb) {
    int flags = ::fcntl(toNative(fd), F_GETFL, 0);
    if (flags < 0) return false;
    if (nb) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return ::fcntl(toNative(fd), F_SETFL, flags) == 0;
}
inline bool lastSocketWouldBlock() {
    return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
}
#endif

bool isCanceled(const std::atomic<bool>* flag) {
    return flag && flag->load(std::memory_order_relaxed);
}

// Scoped RAII guards to guarantee libssh2 handles never leak on any exit path
struct SftpHandleGuard {
    LIBSSH2_SFTP_HANDLE* h = nullptr;
    ~SftpHandleGuard() {
        if (h) libssh2_sftp_close_handle(h);
    }
};

struct ChannelGuard {
    LIBSSH2_CHANNEL* ch = nullptr;
    ~ChannelGuard() {
        if (ch) {
            libssh2_channel_close(ch);
            libssh2_channel_free(ch);
        }
    }
};

struct KbiAuthContext {
    QByteArray password;
};

void kbiCallback(const char* /*name*/,
                 int /*nameLen*/,
                 const char* /*instruction*/,
                 int /*instructionLen*/,
                 int numPrompts,
                 const LIBSSH2_USERAUTH_KBDINT_PROMPT* /*prompts*/,
                 LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
                 void** abstract) {
    if (!abstract || !*abstract) return;
    auto* ctx = static_cast<KbiAuthContext*>(*abstract);
    for (int i = 0; i < numPrompts; ++i) {
        char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(ctx->password.size()) + 1));
        if (buf) {
            std::memcpy(buf, ctx->password.constData(), static_cast<size_t>(ctx->password.size()));
            buf[ctx->password.size()] = '\0';
            responses[i].text = buf;
            responses[i].length = static_cast<unsigned int>(ctx->password.size());
        }
    }
}

int64_t connectTcpWithTimeout(const QString& host,
                              uint16_t port,
                              int timeoutMs,
                              const std::atomic<bool>* cancelFlag,
                              QString& errorOut) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const QByteArray hostBytes = host.toUtf8();
    const QByteArray portBytes = QByteArray::number(port);
    struct addrinfo* res = nullptr;
    int gai = ::getaddrinfo(hostBytes.constData(), portBytes.constData(), &hints, &res);
    if (gai != 0 || !res) {
        errorOut = QStringLiteral("Failed to resolve host '%1': %2")
                       .arg(host, QString::fromUtf8(gai_strerror(gai)));
        return kInvalidSocket;
    }

    int64_t sock = kInvalidSocket;
    QElapsedTimer timer;
    timer.start();

    for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Connection canceled");
            break;
        }
        NativeSocket s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
#ifdef _WIN32
        if (s == INVALID_SOCKET) continue;
#else
        if (s < 0) continue;
#endif
        int64_t cand = static_cast<int64_t>(s);
        setNonBlocking(cand, true);

        int flag = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

        int rc = ::connect(s, rp->ai_addr, static_cast<int>(rp->ai_addrlen));
        if (rc == 0) {
            sock = cand;
            break;
        }
        if (!lastSocketWouldBlock()) {
            closeSocket(cand);
            continue;
        }

        // Poll until writable, timeout, or canceled
        bool connected = false;
        while (timer.elapsed() < timeoutMs) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Connection canceled");
                break;
            }
            int slice = static_cast<int>(std::min<qint64>(100, timeoutMs - timer.elapsed()));
            if (slice <= 0) break;
#ifdef _WIN32
            WSAPOLLFD pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            int prc = ::WSAPoll(&pfd, 1, slice);
#else
            struct pollfd pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            int prc = ::poll(&pfd, 1, slice);
#endif
            if (prc > 0) {
                int soErr = 0;
                socklen_t len = sizeof(soErr);
                if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) == 0 &&
                    soErr == 0) {
                    connected = true;
                }
                break;
            }
            if (prc < 0) break;
        }

        if (connected) {
            sock = cand;
            break;
        }
        closeSocket(cand);
    }

    ::freeaddrinfo(res);

    if (sock == kInvalidSocket && errorOut.isEmpty()) {
        errorOut = QStringLiteral("TCP connection to %1:%2 failed or timed out").arg(host).arg(port);
    }
    return sock;
}

std::mutex& initMutex() {
    static std::mutex m;
    return m;
}
int gInitRefCount = 0;

}  // namespace

struct SshClient::Impl {};

SshGlobalInit::SshGlobalInit() {
    std::lock_guard<std::mutex> lock(initMutex());
    if (gInitRefCount == 0) {
#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        libssh2_init(0);
    }
    ++gInitRefCount;
}

SshGlobalInit::~SshGlobalInit() {
    std::lock_guard<std::mutex> lock(initMutex());
    --gInitRefCount;
    if (gInitRefCount == 0) {
        libssh2_exit();
#ifdef _WIN32
        WSACleanup();
#endif
    }
}

SshClient::SshClient() : impl_(std::make_unique<Impl>()) {}

SshClient::~SshClient() {
    disconnect();
}

void SshClient::disconnect() {
    if (sftp_) {
        libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
    }
    if (session_) {
        libssh2_session_disconnect(session_, "xsync closing");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    closeSocket(socketFd_);

    // Shut down jump host tunnel cleanly
    tunnelStop_.store(true, std::memory_order_relaxed);
    closeSocket(listenFd_);
    if (tunnelThread_.joinable()) {
        tunnelThread_.join();
    }
    if (jumpClient_) {
        jumpClient_->disconnect();
        jumpClient_.reset();
    }
}

bool SshClient::isConnected() const {
    return session_ != nullptr && socketFd_ != kInvalidSocket;
}

QString SshClient::lastSshError(const QString& fallback) const {
    if (!session_) return fallback;
    char* msg = nullptr;
    int len = 0;
    int err = libssh2_session_last_error(session_, &msg, &len, 0);
    if (msg && len > 0) {
        return QStringLiteral("%1 (libssh2 %2: %3)").arg(fallback).arg(err).arg(QString::fromUtf8(msg, len));
    }
    return QStringLiteral("%1 (libssh2 error %2)").arg(fallback).arg(err);
}

QString SshClient::lastSftpError(const QString& fallback) const {
    if (!sftp_) return lastSshError(fallback);
    unsigned long sftpErr = libssh2_sftp_last_error(sftp_);
    if (sftpErr != 0) {
        return QStringLiteral("%1 (SFTP code %2)").arg(fallback).arg(sftpErr);
    }
    return lastSshError(fallback);
}

bool SshClient::waitSocket(int timeoutMs, const std::atomic<bool>* cancelFlag) {
    if (!session_ || socketFd_ == kInvalidSocket) return false;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        if (isCanceled(cancelFlag)) return false;

        int dir = libssh2_session_block_directions(session_);
        short events = 0;
        if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) events |= POLLIN;
        if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) events |= POLLOUT;
        if (events == 0) events = POLLIN;

        int slice = static_cast<int>(std::min<qint64>(80, timeoutMs - timer.elapsed()));
        if (slice <= 0) break;

#ifdef _WIN32
        WSAPOLLFD pfd{};
        pfd.fd = toNative(socketFd_);
        pfd.events = events;
        int rc = ::WSAPoll(&pfd, 1, slice);
#else
        struct pollfd pfd{};
        pfd.fd = toNative(socketFd_);
        pfd.events = events;
        int rc = ::poll(&pfd, 1, slice);
#endif
        if (rc > 0) return true;
        if (rc < 0) return false;
    }
    return false;
}

bool SshClient::connectToHost(const SessionProfile& profile,
                              QString& errorOut,
                              const std::atomic<bool>* cancelFlag,
                              int timeoutMs) {
    disconnect();

    if (!profile.isValid()) {
        errorOut = QStringLiteral("Invalid SSH profile (host/username/port required)");
        return false;
    }

    QString targetHost = profile.host.trimmed();
    uint16_t targetPort = profile.port;

    // Optional JumpHost / Bastion via local loopback TCP forwarder
    if (profile.useJumpHost && !profile.jumpHost.trimmed().isEmpty()) {
        jumpClient_ = std::make_unique<SshClient>();
        SessionProfile jumpProf;
        jumpProf.host = profile.jumpHost.trimmed();
        jumpProf.port = profile.jumpPort;
        jumpProf.username = profile.jumpUsername.isEmpty() ? profile.username : profile.jumpUsername;
        jumpProf.authType = profile.jumpAuthType;
        jumpProf.password = profile.jumpPassword;
        jumpProf.privateKeyPath = profile.jumpPrivateKeyPath;
        jumpProf.passphrase = profile.jumpPassphrase;
        jumpProf.keepAliveSec = profile.keepAliveSec;

        QString jumpErr;
        if (!jumpClient_->connectToHost(jumpProf, jumpErr, cancelFlag, timeoutMs)) {
            errorOut = QStringLiteral("JumpHost failed: %1").arg(jumpErr);
            disconnect();
            return false;
        }

        // Open loopback listener on ephemeral port
        NativeSocket ls = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
        if (ls == INVALID_SOCKET) {
#else
        if (ls < 0) {
#endif
            errorOut = QStringLiteral("Failed to create local tunnel socket for JumpHost");
            disconnect();
            return false;
        }
        listenFd_ = static_cast<int64_t>(ls);

        struct sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin.sin_port = 0;
        if (::bind(ls, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0 ||
            ::listen(ls, 1) != 0) {
            errorOut = QStringLiteral("Failed to bind local loopback tunnel for JumpHost");
            disconnect();
            return false;
        }
        socklen_t slen = sizeof(sin);
        ::getsockname(ls, reinterpret_cast<struct sockaddr*>(&sin), &slen);
        const uint16_t localTunnelPort = ntohs(sin.sin_port);

        // Open direct-tcpip channel on jumpClient_
        const QByteArray dstHostUtf8 = profile.host.trimmed().toUtf8();
        LIBSSH2_CHANNEL* tunnelCh = nullptr;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs && !isCanceled(cancelFlag)) {
            tunnelCh = libssh2_channel_direct_tcpip_ex(jumpClient_->session_,
                                                       dstHostUtf8.constData(),
                                                       profile.port,
                                                       "127.0.0.1",
                                                       localTunnelPort);
            if (tunnelCh) break;
            if (libssh2_session_last_errno(jumpClient_->session_) == LIBSSH2_ERROR_EAGAIN) {
                jumpClient_->waitSocket(100, cancelFlag);
            } else {
                break;
            }
        }
        if (!tunnelCh) {
            errorOut = jumpClient_->lastSshError(
                QStringLiteral("JumpHost failed to open direct-tcpip tunnel to %1:%2")
                    .arg(profile.host)
                    .arg(profile.port));
            disconnect();
            return false;
        }

        tunnelStop_.store(false, std::memory_order_relaxed);
        int64_t listenCopy = listenFd_;
        int64_t jumpSock = jumpClient_->socketFd_;
        tunnelThread_ = std::thread([this, listenCopy, jumpSock, tunnelCh]() {
            ChannelGuard chGuard{tunnelCh};
            setNonBlocking(listenCopy, true);

            int64_t clientFd = kInvalidSocket;
            while (!tunnelStop_.load(std::memory_order_relaxed)) {
#ifdef _WIN32
                WSAPOLLFD lpfd{};
                lpfd.fd = toNative(listenCopy);
                lpfd.events = POLLIN;
                int lrc = ::WSAPoll(&lpfd, 1, 50);
#else
                struct pollfd lpfd{};
                lpfd.fd = toNative(listenCopy);
                lpfd.events = POLLIN;
                int lrc = ::poll(&lpfd, 1, 50);
#endif
                if (lrc > 0) {
                    NativeSocket accepted = ::accept(toNative(listenCopy), nullptr, nullptr);
#ifdef _WIN32
                    if (accepted != INVALID_SOCKET) {
#else
                    if (accepted >= 0) {
#endif
                        clientFd = static_cast<int64_t>(accepted);
                        setNonBlocking(clientFd, true);
                        break;
                    }
                }
            }
            if (clientFd == kInvalidSocket) return;

            std::vector<char> buf(65536);
            while (!tunnelStop_.load(std::memory_order_relaxed)) {
                // Local -> SSH tunnel
                ssize_t n = ::recv(toNative(clientFd), buf.data(), static_cast<int>(buf.size()), 0);
                if (n > 0) {
                    ssize_t off = 0;
                    while (off < n && !tunnelStop_.load(std::memory_order_relaxed)) {
                        ssize_t w = libssh2_channel_write(tunnelCh, buf.data() + off,
                                                          static_cast<size_t>(n - off));
                        if (w > 0) {
                            off += w;
                        } else if (w == LIBSSH2_ERROR_EAGAIN) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        } else {
                            tunnelStop_.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                } else if (n == 0) {
                    break;
                }

                // SSH tunnel -> Local
                ssize_t r = libssh2_channel_read(tunnelCh, buf.data(), buf.size());
                if (r > 0) {
                    ssize_t off = 0;
                    while (off < r && !tunnelStop_.load(std::memory_order_relaxed)) {
                        ssize_t w = ::send(toNative(clientFd), buf.data() + off,
                                           static_cast<int>(r - off), 0);
                        if (w > 0) {
                            off += w;
                        } else if (lastSocketWouldBlock()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        } else {
                            tunnelStop_.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                } else if (r == 0 && libssh2_channel_eof(tunnelCh)) {
                    break;
                } else if (r < 0 && r != LIBSSH2_ERROR_EAGAIN) {
                    break;
                }

                // Brief poll on both sockets to avoid busy spin
#ifdef _WIN32
                WSAPOLLFD pfds[2]{};
                pfds[0].fd = toNative(clientFd);
                pfds[0].events = POLLIN;
                pfds[1].fd = toNative(jumpSock);
                pfds[1].events = POLLIN;
                ::WSAPoll(pfds, 2, 10);
#else
                struct pollfd pfds[2]{};
                pfds[0].fd = toNative(clientFd);
                pfds[0].events = POLLIN;
                pfds[1].fd = toNative(jumpSock);
                pfds[1].events = POLLIN;
                ::poll(pfds, 2, 10);
#endif
            }
            closeSocket(clientFd);
        });

        targetHost = QStringLiteral("127.0.0.1");
        targetPort = localTunnelPort;
    }

    bool ok = connectRaw(targetHost,
                         targetPort,
                         profile.username.trimmed(),
                         profile.authType,
                         profile.password,
                         ShellUtils::expandTilde(profile.privateKeyPath.trimmed()),
                         profile.passphrase,
                         profile.keepAliveSec,
                         errorOut,
                         cancelFlag,
                         timeoutMs);
    if (!ok) {
        disconnect();
    }
    return ok;
}

bool SshClient::connectRaw(const QString& host,
                           uint16_t port,
                           const QString& username,
                           AuthType authType,
                           const QString& password,
                           const QString& privateKeyPath,
                           const QString& passphrase,
                           int keepAliveSec,
                           QString& errorOut,
                           const std::atomic<bool>* cancelFlag,
                           int timeoutMs) {
    socketFd_ = connectTcpWithTimeout(host, port, timeoutMs, cancelFlag, errorOut);
    if (socketFd_ == kInvalidSocket) {
        return false;
    }

    session_ = libssh2_session_init();
    if (!session_) {
        errorOut = QStringLiteral("Failed to allocate libssh2 session");
        return false;
    }

    libssh2_session_set_blocking(session_, 0);
    libssh2_session_flag(session_, LIBSSH2_FLAG_COMPRESS, 0);

    // SSH Handshake
    int rc = 0;
    QElapsedTimer timer;
    timer.start();
    while ((rc = libssh2_session_handshake(session_, static_cast<libssh2_socket_t>(socketFd_))) ==
           LIBSSH2_ERROR_EAGAIN) {
        if (timer.elapsed() >= timeoutMs || !waitSocket(200, cancelFlag)) {
            errorOut = isCanceled(cancelFlag)
                           ? QStringLiteral("SSH handshake canceled")
                           : QStringLiteral("SSH handshake timed out");
            return false;
        }
    }
    if (rc != 0) {
        errorOut = lastSshError(QStringLiteral("SSH handshake failed"));
        return false;
    }

    if (keepAliveSec > 0) {
        libssh2_keepalive_config(session_, 1, static_cast<unsigned>(keepAliveSec));
    }

    // Authenticate
    const QByteArray userUtf8 = username.toUtf8();
    auto runAuth = [&](auto&& fn) -> int {
        int r = 0;
        QElapsedTimer at;
        at.start();
        while ((r = fn()) == LIBSSH2_ERROR_EAGAIN) {
            if (at.elapsed() >= timeoutMs || !waitSocket(200, cancelFlag)) {
                return LIBSSH2_ERROR_TIMEOUT;
            }
        }
        return r;
    };

    bool authed = false;
    if (authType == AuthType::Password) {
        const QByteArray passUtf8 = password.toUtf8();
        int r = runAuth([&]() {
            return libssh2_userauth_password_ex(session_,
                                                userUtf8.constData(),
                                                static_cast<unsigned int>(userUtf8.size()),
                                                passUtf8.constData(),
                                                static_cast<unsigned int>(passUtf8.size()),
                                                nullptr);
        });
        if (r == 0) {
            authed = true;
        } else {
            // Fallback to keyboard-interactive (common on PAM/macOS SSH servers)
            KbiAuthContext ctx{passUtf8};
            *libssh2_session_abstract(session_) = &ctx;
            int kr = runAuth([&]() {
                return libssh2_userauth_keyboard_interactive_ex(
                    session_,
                    userUtf8.constData(),
                    static_cast<unsigned int>(userUtf8.size()),
                    &kbiCallback);
            });
            *libssh2_session_abstract(session_) = nullptr;
            if (kr == 0) {
                authed = true;
            } else {
                errorOut = lastSshError(QStringLiteral("Password authentication failed"));
            }
        }
    } else if (authType == AuthType::PrivateKey) {
        if (privateKeyPath.isEmpty() || !QFileInfo::exists(privateKeyPath)) {
            errorOut = QStringLiteral("Private key file not found: %1").arg(privateKeyPath);
            return false;
        }
        const QByteArray keyPathLocal = QFile::encodeName(privateKeyPath);
        const QByteArray ppUtf8 = passphrase.toUtf8();
        const char* ppPtr = passphrase.isEmpty() ? nullptr : ppUtf8.constData();
        // Pass nullptr for publickey so libssh2+OpenSSL derives it from the private key
        int r = runAuth([&]() {
            return libssh2_userauth_publickey_fromfile_ex(
                session_,
                userUtf8.constData(),
                static_cast<unsigned int>(userUtf8.size()),
                nullptr,
                keyPathLocal.constData(),
                ppPtr);
        });
        if (r == 0) {
            authed = true;
        } else {
            errorOut = lastSshError(QStringLiteral("Private key authentication failed"));
        }
    } else if (authType == AuthType::SshAgent) {
        LIBSSH2_AGENT* agent = libssh2_agent_init(session_);
        if (!agent) {
            errorOut = QStringLiteral("Failed to initialize SSH agent");
            return false;
        }
        if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
            struct libssh2_agent_publickey* identity = nullptr;
            struct libssh2_agent_publickey* prev = nullptr;
            while (libssh2_agent_get_identity(agent, &identity, prev) == 0) {
                int r = runAuth([&]() {
                    return libssh2_agent_userauth(agent, userUtf8.constData(), identity);
                });
                if (r == 0) {
                    authed = true;
                    break;
                }
                prev = identity;
            }
        }
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        if (!authed) {
            errorOut = QStringLiteral("SSH Agent authentication failed (no matching key or agent not running)");
        }
    }

    return authed;
}

bool SshClient::sendKeepAlive() {
    if (!session_) return false;
    int secsToNext = 0;
    int rc = libssh2_keepalive_send(session_, &secsToNext);
    return rc == 0 || rc == LIBSSH2_ERROR_EAGAIN;
}

bool SshClient::ensureSftp(QString& errorOut, const std::atomic<bool>* cancelFlag) {
    if (sftp_) return true;
    if (!session_) {
        errorOut = QStringLiteral("Not connected");
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        sftp_ = libssh2_sftp_init(session_);
        if (sftp_) return true;
        if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
        } else {
            break;
        }
    }
    errorOut = lastSshError(QStringLiteral("Failed to initialize SFTP subsystem"));
    return false;
}

ExecResult SshClient::execCommand(const QString& command,
                                  const std::atomic<bool>* cancelFlag,
                                  int timeoutMs,
                                  const OutputChunkCallback& onChunk,
                                  bool requestAgentForward) {
    ExecResult res;
    if (!session_) {
        res.errorMessage = QStringLiteral("Not connected");
        return res;
    }

    QElapsedTimer timer;
    timer.start();

    LIBSSH2_CHANNEL* ch = nullptr;
    while (true) {
        if (isCanceled(cancelFlag)) {
            res.errorMessage = QStringLiteral("Canceled");
            return res;
        }
        if (timeoutMs > 0 && timer.elapsed() >= timeoutMs) {
            res.errorMessage = QStringLiteral("Timed out opening exec channel");
            return res;
        }
        ch = libssh2_channel_open_session(session_);
        if (ch) break;
        if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
        } else {
            res.errorMessage = lastSshError(QStringLiteral("Failed to open SSH exec channel"));
            return res;
        }
    }
    ChannelGuard guard{ch};

    if (requestAgentForward) {
        // Best-effort agent forwarding request; ignored if server declines
        int rc = 0;
        while ((rc = libssh2_channel_request_auth_agent(ch)) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) break;
        }
    }

    const QByteArray cmdUtf8 = command.toUtf8();
    int rc = 0;
    while ((rc = libssh2_channel_exec(ch, cmdUtf8.constData())) == LIBSSH2_ERROR_EAGAIN) {
        if (isCanceled(cancelFlag)) {
            res.errorMessage = QStringLiteral("Canceled");
            return res;
        }
        if (timeoutMs > 0 && timer.elapsed() >= timeoutMs) {
            res.errorMessage = QStringLiteral("Timed out starting command");
            return res;
        }
        waitSocket(100, cancelFlag);
    }
    if (rc != 0) {
        res.errorMessage = lastSshError(QStringLiteral("Failed to execute remote command"));
        return res;
    }

    // Read stdout and stderr with bounded accumulation (cap text accumulation at 2 MB to avoid leaks)
    constexpr int kMaxAccumChars = 2 * 1024 * 1024;
    char buf[16384];
    while (true) {
        if (isCanceled(cancelFlag)) {
            res.errorMessage = QStringLiteral("Canceled");
            return res;
        }
        if (timeoutMs > 0 && timer.elapsed() >= timeoutMs) {
            res.errorMessage = QStringLiteral("Command timed out");
            return res;
        }

        bool gotData = false;
        ssize_t nOut = libssh2_channel_read(ch, buf, sizeof(buf));
        if (nOut > 0) {
            gotData = true;
            QString chunk = QString::fromUtf8(buf, static_cast<qsizetype>(nOut));
            if (onChunk) onChunk(chunk, false);
            if (res.stdoutText.size() < kMaxAccumChars) {
                res.stdoutText.append(chunk);
            }
        }

        ssize_t nErr = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
        if (nErr > 0) {
            gotData = true;
            QString chunk = QString::fromUtf8(buf, static_cast<qsizetype>(nErr));
            if (onChunk) onChunk(chunk, true);
            if (res.stderrText.size() < kMaxAccumChars) {
                res.stderrText.append(chunk);
            }
        }

        if (gotData) continue;

        if (nOut == LIBSSH2_ERROR_EAGAIN || nErr == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
            continue;
        }
        if (libssh2_channel_eof(ch)) {
            break;
        }
        if (nOut < 0 || nErr < 0) {
            res.errorMessage = lastSshError(QStringLiteral("Error reading remote command output"));
            return res;
        }
        waitSocket(50, cancelFlag);
    }

    while (libssh2_channel_close(ch) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitSocket(100, cancelFlag)) break;
    }
    res.exitCode = libssh2_channel_get_exit_status(ch);
    res.ok = true;
    return res;
}

QString SshClient::resolveHomeDirectory(QString& errorOut, const std::atomic<bool>* cancelFlag) {
    if (ensureSftp(errorOut, cancelFlag)) {
        char buf[4096];
        int rc = 0;
        QElapsedTimer t;
        t.start();
        while ((rc = libssh2_sftp_realpath(sftp_, ".", buf, sizeof(buf) - 1)) == LIBSSH2_ERROR_EAGAIN) {
            if (t.elapsed() > 8000 || !waitSocket(100, cancelFlag)) break;
        }
        if (rc > 0) {
            buf[rc] = '\0';
            return normalizeRemotePath(QString::fromUtf8(buf, rc));
        }
    }
    // Fallback to exec `pwd`
    ExecResult r = execCommand(QStringLiteral("pwd"), cancelFlag, 5000);
    if (r.ok && r.exitCode == 0 && !r.stdoutText.trimmed().isEmpty()) {
        return normalizeRemotePath(r.stdoutText.trimmed());
    }
    return QStringLiteral("/");
}

bool SshClient::listDirectory(const QString& remotePath,
                              std::vector<FileEntry>& outEntries,
                              QString& errorOut,
                              const std::atomic<bool>* cancelFlag) {
    outEntries.clear();
    const QString normPath = normalizeRemotePath(remotePath);

    if (ensureSftp(errorOut, cancelFlag)) {
        const QByteArray pathUtf8 = normPath.toUtf8();
        LIBSSH2_SFTP_HANDLE* dir = nullptr;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 15000) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            dir = libssh2_sftp_opendir(sftp_, pathUtf8.constData());
            if (dir) break;
            if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
                waitSocket(100, cancelFlag);
            } else {
                break;
            }
        }
        if (!dir) {
            errorOut = lastSftpError(QStringLiteral("Cannot open directory %1").arg(normPath));
            return false;
        }
        SftpHandleGuard guard{dir};

        char nameBuf[1024];
        char longBuf[1024];
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        while (true) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            int rc = libssh2_sftp_readdir_ex(dir, nameBuf, sizeof(nameBuf) - 1,
                                             longBuf, sizeof(longBuf) - 1, &attrs);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                waitSocket(100, cancelFlag);
                continue;
            }
            if (rc == 0) break;
            if (rc < 0) {
                errorOut = lastSftpError(QStringLiteral("Error reading directory %1").arg(normPath));
                return false;
            }
            nameBuf[rc] = '\0';
            QString name = QString::fromUtf8(nameBuf, rc);
            if (name == QLatin1String(".") || name == QLatin1String("..")) continue;

            FileEntry e;
            e.name = name;
            e.fullPath = joinRemotePath(normPath, name);
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) e.size = attrs.filesize;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) e.modifiedEpoch = static_cast<int64_t>(attrs.mtime);
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
                e.permissions = static_cast<uint32_t>(attrs.permissions);
                e.isDir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
                e.isSymlink = LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
            }
            // If symlink, stat target once to know if it points to a directory
            if (e.isSymlink) {
                LIBSSH2_SFTP_ATTRIBUTES targetAttrs{};
                const QByteArray fullUtf8 = e.fullPath.toUtf8();
                int src = 0;
                while ((src = libssh2_sftp_stat(sftp_, fullUtf8.constData(), &targetAttrs)) ==
                       LIBSSH2_ERROR_EAGAIN) {
                    if (!waitSocket(50, cancelFlag)) break;
                }
                if (src == 0 && (targetAttrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
                    e.isDir = LIBSSH2_SFTP_S_ISDIR(targetAttrs.permissions);
                }
            }
            outEntries.push_back(std::move(e));
        }
        return true;
    }

    // Fallback when SFTP subsystem is disabled: list via POSIX shell exec
    const QString cmd = QStringLiteral("LC_ALL=C ls -la --time-style=+%s %1 2>/dev/null || LC_ALL=C ls -la %1")
                            .arg(ShellUtils::shellQuotePosix(normPath));
    ExecResult r = execCommand(cmd, cancelFlag, 10000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    const QStringList lines = r.stdoutText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String("total "))) continue;
        const QStringList cols = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (cols.size() < 7) continue;
        const QString perms = cols[0];
        if (perms.size() < 10) continue;

        // Parse depending on --time-style=+%s (7 cols) vs standard ls -la (9 cols)
        bool isEpoch = false;
        int64_t mtime = cols[5].toLongLong(&isEpoch);
        int nameStartCol = isEpoch ? 6 : 8;
        if (cols.size() <= nameStartCol) continue;

        QString name = cols.mid(nameStartCol).join(QLatin1Char(' '));
        if (perms[0] == QLatin1Char('l') && name.contains(QLatin1String(" -> "))) {
            name = name.section(QLatin1String(" -> "), 0, 0);
        }
        if (name == QLatin1String(".") || name == QLatin1String("..")) continue;

        FileEntry e;
        e.name = name;
        e.fullPath = joinRemotePath(normPath, name);
        e.isDir = (perms[0] == QLatin1Char('d'));
        e.isSymlink = (perms[0] == QLatin1Char('l'));
        e.size = cols[4].toULongLong();
        e.modifiedEpoch = isEpoch ? mtime : 0;
        uint32_t mode = 0;
        const uint32_t bits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
        for (int i = 0; i < 9; ++i) {
            if (perms[i + 1] != QLatin1Char('-')) mode |= bits[i];
        }
        e.permissions = mode;
        outEntries.push_back(std::move(e));
    }
    return true;
}

bool SshClient::statPath(const QString& remotePath,
                         FileEntry& outEntry,
                         QString& errorOut,
                         const std::atomic<bool>* cancelFlag) {
    const QString norm = normalizeRemotePath(remotePath);
    if (ensureSftp(errorOut, cancelFlag)) {
        const QByteArray p = norm.toUtf8();
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        int rc = 0;
        while ((rc = libssh2_sftp_stat(sftp_, p.constData(), &attrs)) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) {
                errorOut = QStringLiteral("Timed out stating path");
                return false;
            }
        }
        if (rc != 0) {
            errorOut = lastSftpError(QStringLiteral("stat failed for %1").arg(norm));
            return false;
        }
        outEntry.name = baseNameAnyPath(norm);
        outEntry.fullPath = norm;
        outEntry.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
        outEntry.modifiedEpoch = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? static_cast<int64_t>(attrs.mtime) : 0;
        outEntry.permissions = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? static_cast<uint32_t>(attrs.permissions) : 0644;
        outEntry.isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        outEntry.isSymlink = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
        return true;
    }

    ExecResult r = execCommand(QStringLiteral("stat -c '%F|%s|%Y|%a' %1").arg(ShellUtils::shellQuotePosix(norm)),
                               cancelFlag, 5000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    const QStringList parts = r.stdoutText.trimmed().split(QLatin1Char('|'));
    if (parts.size() < 4) return false;
    outEntry.name = baseNameAnyPath(norm);
    outEntry.fullPath = norm;
    outEntry.isDir = parts[0].contains(QLatin1String("directory"), Qt::CaseInsensitive);
    outEntry.size = parts[1].toULongLong();
    outEntry.modifiedEpoch = parts[2].toLongLong();
    outEntry.permissions = parts[3].toUInt(nullptr, 8);
    return true;
}

bool SshClient::mkdir(const QString& remotePath,
                      uint32_t mode,
                      QString& errorOut,
                      const std::atomic<bool>* cancelFlag) {
    const QString norm = normalizeRemotePath(remotePath);
    if (ensureSftp(errorOut, cancelFlag)) {
        const QByteArray p = norm.toUtf8();
        int rc = 0;
        while ((rc = libssh2_sftp_mkdir(sftp_, p.constData(), static_cast<long>(mode))) ==
               LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) return false;
        }
        if (rc == 0) return true;
        // If already exists as a directory, treat as success
        FileEntry st;
        QString stErr;
        if (statPath(norm, st, stErr, cancelFlag) && st.isDir) return true;
        errorOut = lastSftpError(QStringLiteral("mkdir failed for %1").arg(norm));
        return false;
    }
    ExecResult r = execCommand(QStringLiteral("mkdir -p %1").arg(ShellUtils::shellQuotePosix(norm)),
                               cancelFlag, 5000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    return true;
}

bool SshClient::mkdirRecursive(const QString& remotePath,
                               QString& errorOut,
                               const std::atomic<bool>* cancelFlag) {
    const QString norm = normalizeRemotePath(remotePath);
    if (norm == QLatin1String("/")) return true;

    FileEntry st;
    QString dummy;
    if (statPath(norm, st, dummy, cancelFlag) && st.isDir) return true;

    const QStringList parts = norm.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString cur;
    for (const QString& part : parts) {
        cur += QLatin1Char('/') + part;
        if (statPath(cur, st, dummy, cancelFlag) && st.isDir) continue;
        if (!mkdir(cur, 0755, errorOut, cancelFlag)) return false;
    }
    return true;
}

bool SshClient::renamePath(const QString& oldPath,
                           const QString& newPath,
                           QString& errorOut,
                           const std::atomic<bool>* cancelFlag) {
    const QString o = normalizeRemotePath(oldPath);
    const QString n = normalizeRemotePath(newPath);
    if (ensureSftp(errorOut, cancelFlag)) {
        const QByteArray oUtf8 = o.toUtf8();
        const QByteArray nUtf8 = n.toUtf8();
        int rc = 0;
        while ((rc = libssh2_sftp_rename_ex(sftp_,
                                            oUtf8.constData(), static_cast<unsigned>(oUtf8.size()),
                                            nUtf8.constData(), static_cast<unsigned>(nUtf8.size()),
                                            LIBSSH2_SFTP_RENAME_OVERWRITE |
                                                LIBSSH2_SFTP_RENAME_ATOMIC |
                                                LIBSSH2_SFTP_RENAME_NATIVE)) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) return false;
        }
        if (rc == 0) return true;
        errorOut = lastSftpError(QStringLiteral("Rename failed"));
        return false;
    }
    ExecResult r = execCommand(QStringLiteral("mv -f %1 %2")
                                   .arg(ShellUtils::shellQuotePosix(o), ShellUtils::shellQuotePosix(n)),
                               cancelFlag, 10000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    return true;
}

bool SshClient::removeFile(const QString& remotePath,
                           QString& errorOut,
                           const std::atomic<bool>* cancelFlag) {
    const QString norm = normalizeRemotePath(remotePath);
    if (ensureSftp(errorOut, cancelFlag)) {
        const QByteArray p = norm.toUtf8();
        int rc = 0;
        while ((rc = libssh2_sftp_unlink(sftp_, p.constData())) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) return false;
        }
        if (rc == 0) return true;
        errorOut = lastSftpError(QStringLiteral("Delete file failed for %1").arg(norm));
        return false;
    }
    ExecResult r = execCommand(QStringLiteral("rm -f %1").arg(ShellUtils::shellQuotePosix(norm)),
                               cancelFlag, 10000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    return true;
}

bool SshClient::removeDirRecursive(const QString& remotePath,
                                   QString& errorOut,
                                   const std::atomic<bool>* cancelFlag) {
    const QString norm = normalizeRemotePath(remotePath);
    if (norm == QLatin1String("/")) {
        errorOut = QStringLiteral("Refusing to delete root directory '/'");
        return false;
    }
    if (ensureSftp(errorOut, cancelFlag)) {
        std::vector<FileEntry> children;
        if (!listDirectory(norm, children, errorOut, cancelFlag)) return false;
        for (const FileEntry& c : children) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            if (c.isDir && !c.isSymlink) {
                if (!removeDirRecursive(c.fullPath, errorOut, cancelFlag)) return false;
            } else {
                if (!removeFile(c.fullPath, errorOut, cancelFlag)) return false;
            }
        }
        const QByteArray p = norm.toUtf8();
        int rc = 0;
        while ((rc = libssh2_sftp_rmdir(sftp_, p.constData())) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) return false;
        }
        if (rc == 0) return true;
        errorOut = lastSftpError(QStringLiteral("rmdir failed for %1").arg(norm));
        return false;
    }
    ExecResult r = execCommand(QStringLiteral("rm -rf %1").arg(ShellUtils::shellQuotePosix(norm)),
                               cancelFlag, 30000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    return true;
}

bool SshClient::chmodPath(const QString& remotePath,
                          uint32_t mode,
                          QString& errorOut,
                          const std::atomic<bool>* cancelFlag) {
    const QString norm = normalizeRemotePath(remotePath);
    if (ensureSftp(errorOut, cancelFlag)) {
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        attrs.permissions = mode;
        const QByteArray p = norm.toUtf8();
        int rc = 0;
        while ((rc = libssh2_sftp_setstat(sftp_, p.constData(), &attrs)) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(100, cancelFlag)) return false;
        }
        if (rc == 0) return true;
        errorOut = lastSftpError(QStringLiteral("chmod failed"));
        return false;
    }
    ExecResult r = execCommand(QStringLiteral("chmod %1 %2")
                                   .arg(QString::number(mode & 07777, 8), ShellUtils::shellQuotePosix(norm)),
                               cancelFlag, 5000);
    if (!r.ok || r.exitCode != 0) {
        errorOut = r.stderrText.isEmpty() ? r.errorMessage : r.stderrText.trimmed();
        return false;
    }
    return true;
}

bool SshClient::uploadFileSftp(const QString& localPath,
                               const QString& remotePath,
                               const ProgressCallback& progressCb,
                               const std::atomic<bool>* cancelFlag,
                               QString& errorOut) {
    if (!ensureSftp(errorOut, cancelFlag)) return false;

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        errorOut = QStringLiteral("Cannot open local file '%1': %2").arg(localPath, localFile.errorString());
        return false;
    }
    const uint64_t totalBytes = static_cast<uint64_t>(localFile.size());

    const QString normRemote = normalizeRemotePath(remotePath);
    const QByteArray remoteUtf8 = normRemote.toUtf8();
    LIBSSH2_SFTP_HANDLE* fh = nullptr;
    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        fh = libssh2_sftp_open(sftp_, remoteUtf8.constData(),
                               LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                               LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                                   LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
        if (fh) break;
        if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
        } else {
            errorOut = lastSftpError(QStringLiteral("Cannot open remote file for write: %1").arg(normRemote));
            return false;
        }
    }
    SftpHandleGuard guard{fh};

    // Single fixed 256 KiB reusable buffer -> strictly flat memory footprint
    std::vector<char> buffer(kTransferBufferSize);
    uint64_t sent = 0;

    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const qint64 nRead = localFile.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (nRead < 0) {
            errorOut = QStringLiteral("Local read error: %1").arg(localFile.errorString());
            return false;
        }
        if (nRead == 0) break;

        qint64 offset = 0;
        while (offset < nRead) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            ssize_t nWritten = libssh2_sftp_write(fh, buffer.data() + offset,
                                                  static_cast<size_t>(nRead - offset));
            if (nWritten == LIBSSH2_ERROR_EAGAIN) {
                waitSocket(100, cancelFlag);
                continue;
            }
            if (nWritten < 0) {
                errorOut = lastSftpError(QStringLiteral("SFTP write failed"));
                return false;
            }
            offset += nWritten;
            sent += static_cast<uint64_t>(nWritten);
            if (progressCb) progressCb(sent, totalBytes);
        }
    }
    if (progressCb) progressCb(totalBytes, totalBytes);
    return true;
}

bool SshClient::downloadFileSftp(const QString& remotePath,
                                 const QString& localPath,
                                 const ProgressCallback& progressCb,
                                 const std::atomic<bool>* cancelFlag,
                                 QString& errorOut) {
    if (!ensureSftp(errorOut, cancelFlag)) return false;

    const QString normRemote = normalizeRemotePath(remotePath);
    const QByteArray remoteUtf8 = normRemote.toUtf8();

    LIBSSH2_SFTP_HANDLE* fh = nullptr;
    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        fh = libssh2_sftp_open(sftp_, remoteUtf8.constData(), LIBSSH2_FXF_READ, 0);
        if (fh) break;
        if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
        } else {
            errorOut = lastSftpError(QStringLiteral("Cannot open remote file for read: %1").arg(normRemote));
            return false;
        }
    }
    SftpHandleGuard guard{fh};

    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    uint64_t totalBytes = 0;
    int statRc = 0;
    while ((statRc = libssh2_sftp_fstat(fh, &attrs)) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitSocket(100, cancelFlag)) break;
    }
    if (statRc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
        totalBytes = attrs.filesize;
    }

    QFileInfo lfi(localPath);
    QDir().mkpath(lfi.absolutePath());
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorOut = QStringLiteral("Cannot open local file '%1' for write: %2")
                       .arg(localPath, localFile.errorString());
        return false;
    }

    std::vector<char> buffer(kTransferBufferSize);
    uint64_t received = 0;

    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        ssize_t nRead = libssh2_sftp_read(fh, buffer.data(), buffer.size());
        if (nRead == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
            continue;
        }
        if (nRead == 0) break;
        if (nRead < 0) {
            errorOut = lastSftpError(QStringLiteral("SFTP read failed"));
            return false;
        }
        if (localFile.write(buffer.data(), static_cast<qint64>(nRead)) != nRead) {
            errorOut = QStringLiteral("Local write error: %1").arg(localFile.errorString());
            return false;
        }
        received += static_cast<uint64_t>(nRead);
        if (progressCb) progressCb(received, std::max(totalBytes, received));
    }
    localFile.flush();
    if (progressCb) progressCb(received, std::max(totalBytes, received));
    return true;
}

bool SshClient::uploadFileScp(const QString& localPath,
                              const QString& remotePath,
                              const ProgressCallback& progressCb,
                              const std::atomic<bool>* cancelFlag,
                              QString& errorOut) {
    if (!session_) {
        errorOut = QStringLiteral("Not connected");
        return false;
    }
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        errorOut = QStringLiteral("Cannot open local file '%1'").arg(localPath);
        return false;
    }
    const uint64_t totalBytes = static_cast<uint64_t>(localFile.size());
    const QString normRemote = normalizeRemotePath(remotePath);
    const QByteArray remoteUtf8 = normRemote.toUtf8();

    LIBSSH2_CHANNEL* ch = nullptr;
    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        ch = libssh2_scp_send64(session_, remoteUtf8.constData(), 0644,
                                static_cast<libssh2_int64_t>(totalBytes), 0, 0);
        if (ch) break;
        if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
        } else {
            errorOut = lastSshError(QStringLiteral("SCP send init failed for %1").arg(normRemote));
            return false;
        }
    }
    ChannelGuard guard{ch};

    std::vector<char> buffer(kTransferBufferSize);
    uint64_t sent = 0;

    while (sent < totalBytes) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const qint64 nRead = localFile.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (nRead <= 0) break;

        qint64 offset = 0;
        while (offset < nRead) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            ssize_t w = libssh2_channel_write(ch, buffer.data() + offset,
                                              static_cast<size_t>(nRead - offset));
            if (w == LIBSSH2_ERROR_EAGAIN) {
                waitSocket(100, cancelFlag);
                continue;
            }
            if (w < 0) {
                errorOut = lastSshError(QStringLiteral("SCP write failed"));
                return false;
            }
            offset += w;
            sent += static_cast<uint64_t>(w);
            if (progressCb) progressCb(sent, totalBytes);
        }
    }

    while (libssh2_channel_send_eof(ch) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitSocket(100, cancelFlag)) break;
    }
    while (libssh2_channel_wait_eof(ch) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitSocket(100, cancelFlag)) break;
    }
    while (libssh2_channel_wait_closed(ch) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitSocket(100, cancelFlag)) break;
    }
    if (progressCb) progressCb(totalBytes, totalBytes);
    return true;
}

bool SshClient::downloadFileScp(const QString& remotePath,
                                const QString& localPath,
                                const ProgressCallback& progressCb,
                                const std::atomic<bool>* cancelFlag,
                                QString& errorOut) {
    if (!session_) {
        errorOut = QStringLiteral("Not connected");
        return false;
    }
    const QString normRemote = normalizeRemotePath(remotePath);
    const QByteArray remoteUtf8 = normRemote.toUtf8();

    libssh2_struct_stat st{};
    LIBSSH2_CHANNEL* ch = nullptr;
    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        ch = libssh2_scp_recv2(session_, remoteUtf8.constData(), &st);
        if (ch) break;
        if (libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
        } else {
            errorOut = lastSshError(QStringLiteral("SCP recv init failed for %1").arg(normRemote));
            return false;
        }
    }
    ChannelGuard guard{ch};

    const uint64_t totalBytes = static_cast<uint64_t>(std::max<int64_t>(0, static_cast<int64_t>(st.st_size)));
    QFileInfo lfi(localPath);
    QDir().mkpath(lfi.absolutePath());
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorOut = QStringLiteral("Cannot open local file '%1' for write").arg(localPath);
        return false;
    }

    std::vector<char> buffer(kTransferBufferSize);
    uint64_t received = 0;

    while (received < totalBytes) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), totalBytes - received));
        ssize_t nRead = libssh2_channel_read(ch, buffer.data(), want);
        if (nRead == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, cancelFlag);
            continue;
        }
        if (nRead == 0) break;
        if (nRead < 0) {
            errorOut = lastSshError(QStringLiteral("SCP read failed"));
            return false;
        }
        if (localFile.write(buffer.data(), static_cast<qint64>(nRead)) != nRead) {
            errorOut = QStringLiteral("Local write error");
            return false;
        }
        received += static_cast<uint64_t>(nRead);
        if (progressCb) progressCb(received, totalBytes);
    }
    localFile.flush();
    if (progressCb) progressCb(totalBytes, totalBytes);
    return true;
}

bool SshClient::relayFileStream(SshClient& srcClient,
                                const QString& srcRemotePath,
                                bool srcUseSftp,
                                SshClient& dstClient,
                                const QString& dstRemotePath,
                                bool dstUseSftp,
                                const ProgressCallback& progressCb,
                                const std::atomic<bool>* cancelFlag,
                                QString& errorOut) {
    const QString normSrc = normalizeRemotePath(srcRemotePath);
    const QString normDst = normalizeRemotePath(dstRemotePath);
    const QByteArray srcUtf8 = normSrc.toUtf8();
    const QByteArray dstUtf8 = normDst.toUtf8();

    // Open Source Handle (SFTP or SCP)
    SftpHandleGuard srcSftpGuard;
    ChannelGuard srcScpGuard;
    uint64_t totalBytes = 0;

    if (srcUseSftp) {
        if (!srcClient.ensureSftp(errorOut, cancelFlag)) return false;
        while (true) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            srcSftpGuard.h = libssh2_sftp_open(srcClient.sftp_, srcUtf8.constData(), LIBSSH2_FXF_READ, 0);
            if (srcSftpGuard.h) break;
            if (libssh2_session_last_errno(srcClient.session_) == LIBSSH2_ERROR_EAGAIN) {
                srcClient.waitSocket(100, cancelFlag);
            } else {
                errorOut = srcClient.lastSftpError(QStringLiteral("Source SFTP open failed"));
                return false;
            }
        }
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        int stRc = 0;
        while ((stRc = libssh2_sftp_fstat(srcSftpGuard.h, &attrs)) == LIBSSH2_ERROR_EAGAIN) {
            if (!srcClient.waitSocket(100, cancelFlag)) break;
        }
        if (stRc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
            totalBytes = attrs.filesize;
        }
    } else {
        libssh2_struct_stat st{};
        while (true) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            srcScpGuard.ch = libssh2_scp_recv2(srcClient.session_, srcUtf8.constData(), &st);
            if (srcScpGuard.ch) break;
            if (libssh2_session_last_errno(srcClient.session_) == LIBSSH2_ERROR_EAGAIN) {
                srcClient.waitSocket(100, cancelFlag);
            } else {
                errorOut = srcClient.lastSshError(QStringLiteral("Source SCP open failed"));
                return false;
            }
        }
        totalBytes = static_cast<uint64_t>(std::max<int64_t>(0, static_cast<int64_t>(st.st_size)));
    }

    // Open Destination Handle (SFTP or SCP)
    SftpHandleGuard dstSftpGuard;
    ChannelGuard dstScpGuard;

    if (dstUseSftp) {
        if (!dstClient.ensureSftp(errorOut, cancelFlag)) return false;
        while (true) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            dstSftpGuard.h = libssh2_sftp_open(
                dstClient.sftp_, dstUtf8.constData(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                    LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
            if (dstSftpGuard.h) break;
            if (libssh2_session_last_errno(dstClient.session_) == LIBSSH2_ERROR_EAGAIN) {
                dstClient.waitSocket(100, cancelFlag);
            } else {
                errorOut = dstClient.lastSftpError(QStringLiteral("Destination SFTP open failed"));
                return false;
            }
        }
    } else {
        while (true) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            dstScpGuard.ch = libssh2_scp_send64(dstClient.session_, dstUtf8.constData(), 0644,
                                                static_cast<libssh2_int64_t>(totalBytes), 0, 0);
            if (dstScpGuard.ch) break;
            if (libssh2_session_last_errno(dstClient.session_) == LIBSSH2_ERROR_EAGAIN) {
                dstClient.waitSocket(100, cancelFlag);
            } else {
                errorOut = dstClient.lastSshError(QStringLiteral("Destination SCP open failed"));
                return false;
            }
        }
    }

    // Zero-disk-buffer, fixed 256 KiB streaming relay loop
    std::vector<char> buffer(kTransferBufferSize);
    uint64_t relayed = 0;

    while (true) {
        if (isCanceled(cancelFlag)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        if (!srcUseSftp && relayed >= totalBytes) break;

        ssize_t nRead = 0;
        if (srcUseSftp) {
            nRead = libssh2_sftp_read(srcSftpGuard.h, buffer.data(), buffer.size());
        } else {
            size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), totalBytes - relayed));
            nRead = libssh2_channel_read(srcScpGuard.ch, buffer.data(), want);
        }

        if (nRead == LIBSSH2_ERROR_EAGAIN) {
            srcClient.waitSocket(100, cancelFlag);
            continue;
        }
        if (nRead == 0) break;
        if (nRead < 0) {
            errorOut = srcUseSftp ? srcClient.lastSftpError(QStringLiteral("Relay source read failed"))
                                  : srcClient.lastSshError(QStringLiteral("Relay source SCP read failed"));
            return false;
        }

        ssize_t offset = 0;
        while (offset < nRead) {
            if (isCanceled(cancelFlag)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            ssize_t nWritten = 0;
            if (dstUseSftp) {
                nWritten = libssh2_sftp_write(dstSftpGuard.h, buffer.data() + offset,
                                              static_cast<size_t>(nRead - offset));
            } else {
                nWritten = libssh2_channel_write(dstScpGuard.ch, buffer.data() + offset,
                                                 static_cast<size_t>(nRead - offset));
            }
            if (nWritten == LIBSSH2_ERROR_EAGAIN) {
                dstClient.waitSocket(100, cancelFlag);
                continue;
            }
            if (nWritten < 0) {
                errorOut = dstUseSftp ? dstClient.lastSftpError(QStringLiteral("Relay destination write failed"))
                                      : dstClient.lastSshError(QStringLiteral("Relay destination SCP write failed"));
                return false;
            }
            offset += nWritten;
            relayed += static_cast<uint64_t>(nWritten);
            if (progressCb) progressCb(relayed, std::max(totalBytes, relayed));
        }
    }

    if (!dstUseSftp && dstScpGuard.ch) {
        while (libssh2_channel_send_eof(dstScpGuard.ch) == LIBSSH2_ERROR_EAGAIN) {
            if (!dstClient.waitSocket(100, cancelFlag)) break;
        }
        while (libssh2_channel_wait_eof(dstScpGuard.ch) == LIBSSH2_ERROR_EAGAIN) {
            if (!dstClient.waitSocket(100, cancelFlag)) break;
        }
        while (libssh2_channel_wait_closed(dstScpGuard.ch) == LIBSSH2_ERROR_EAGAIN) {
            if (!dstClient.waitSocket(100, cancelFlag)) break;
        }
    }

    if (progressCb) progressCb(relayed, std::max(totalBytes, relayed));
    return true;
}

}  // namespace xsync
