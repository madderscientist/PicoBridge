#include "ws_client.h"

#include <android/log.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PicoBridge/ws", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "PicoBridge/ws", __VA_ARGS__)

namespace {

// 阻塞式 connect 对不可达地址要等满 TCP SYN 重试（约 2 分钟），必须自己控超时
int connectWithTimeout(addrinfo *ai, int timeoutMs) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) return -1;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, timeoutMs) <= 0) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    return fd;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += (i + 1 < len) ? kB64[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? kB64[n & 63] : '=';
    }
    return out;
}

// 解析 ws://host[:port][/path]
bool parseUrl(const std::string &url, std::string &host, std::string &port, std::string &path) {
    const std::string prefix = "ws://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    std::string rest = url.substr(prefix.size());
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        host = authority;
        port = "80";
    } else {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    return !host.empty() && !port.empty();
}

}  // namespace

WsClient::~WsClient() { stop(); }

void WsClient::start(std::string url) {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(urlMtx_);
        url_ = std::move(url);
    }
    rngState_ ^= static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    thread_ = std::thread(&WsClient::run, this);
}

void WsClient::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    breakSocket();
    if (thread_.joinable()) thread_.join();
}

void WsClient::setUrl(std::string url) {
    {
        std::lock_guard<std::mutex> lock(urlMtx_);
        if (url == url_) return;
        url_ = std::move(url);
    }
    urlDirty_.store(true);
    cv_.notify_all();
    breakSocket();
}

// 从外部线程打断阻塞中的 socket，让工作线程立刻跳出当前连接
void WsClient::breakSocket() {
    const int f = fd_.load();
    if (f >= 0) ::shutdown(f, SHUT_RDWR);
}

void WsClient::send(const std::string &text) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_ = text;
        hasPending_ = true;
    }
    cv_.notify_one();
}

bool WsClient::writeAll(int fd, const void *data, size_t len) const {
    const auto *p = static_cast<const uint8_t *>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool WsClient::handshake(int fd, const std::string &host, const std::string &port,
                         const std::string &path) {
    uint8_t nonce[16];
    for (unsigned char &b : nonce) {
        rngState_ ^= rngState_ << 13;
        rngState_ ^= rngState_ >> 17;
        rngState_ ^= rngState_ << 5;
        b = static_cast<uint8_t>(rngState_);
    }
    std::string req = "GET " + path +
                      " HTTP/1.1\r\n"
                      "Host: " + host + ":" + port + "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + base64(nonce, sizeof(nonce)) + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!writeAll(fd, req.data(), req.size())) return false;

    // 读到响应头结束为止；服务端是 aiohttp，握手响应很小
    std::string resp;
    char buf[512];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 5000) <= 0) return false;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.size() > 8192) return false;
    }
    size_t end = resp.find("\r\n\r\n") + 4;
    rbuf_.assign(resp, end, std::string::npos);  // 握手后可能已粘上数据帧
    return resp.find(" 101 ") != std::string::npos;
}

bool WsClient::sendFrame(int fd, uint8_t opcode, const std::string &payload) {
    std::string &frame = frameBuf_;
    frame.clear();
    frame += static_cast<char>(0x80 | opcode);  // FIN

    const size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(0x80 | len);
    } else if (len < 65536) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) frame += static_cast<char>((len >> (i * 8)) & 0xFF);
    }

    // 客户端发出的帧必须掩码
    uint8_t mask[4];
    for (unsigned char &m : mask) {
        rngState_ ^= rngState_ << 13;
        rngState_ ^= rngState_ >> 17;
        rngState_ ^= rngState_ << 5;
        m = static_cast<uint8_t>(rngState_);
        frame += static_cast<char>(m);
    }
    for (size_t i = 0; i < len; ++i) {
        frame += static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
    }
    return writeAll(fd, frame.data(), frame.size());
}

bool WsClient::pumpIncoming(int fd) {
    pollfd pfd{fd, POLLIN, 0};
    while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char buf[2048];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rbuf_.append(buf, static_cast<size_t>(n));
        if (rbuf_.size() > (1u << 20)) return false;
    }

    for (;;) {
        if (rbuf_.size() < 2) return true;
        const auto *p = reinterpret_cast<const uint8_t *>(rbuf_.data());
        const uint8_t opcode = p[0] & 0x0F;
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t offset = 2;
        if (len == 126) {
            if (rbuf_.size() < 4) return true;
            len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
            offset = 4;
        } else if (len == 127) {
            if (rbuf_.size() < 10) return true;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            offset = 10;
        }
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (rbuf_.size() < offset + 4) return true;
            std::memcpy(mask, p + offset, 4);
            offset += 4;
        }
        if (rbuf_.size() < offset + len) return true;

        std::string payload = rbuf_.substr(offset, static_cast<size_t>(len));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
        }
        rbuf_.erase(0, offset + static_cast<size_t>(len));

        if (opcode == 0x8) return false;                       // close
        if (opcode == 0x9 && !sendFrame(fd, 0x0A, payload)) return false;  // ping -> pong
        if (opcode == 0x1 && onText_) onText_(payload);
    }
}

void WsClient::run() {
    while (running_.load()) {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(urlMtx_);
            url = url_;
        }
        urlDirty_.store(false);

        std::string host, port, path;
        if (!parseUrl(url, host, port, path)) {
            LOGW("bad url: %s", url.c_str());
            waitBeforeRetry();
            continue;
        }
        LOGI("target ws://%s:%s%s", host.c_str(), port.c_str(), path.c_str());

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int fd = -1;

        if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0) {
            for (addrinfo *ai = res; ai != nullptr && fd < 0; ai = ai->ai_next) {
                fd = connectWithTimeout(ai, 3000);
            }
            ::freeaddrinfo(res);
        }

        if (fd < 0) {
            waitBeforeRetry();
            continue;
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval tv{3, 0};  // 握手读不到响应时的保底，避免永久卡在 recv
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        rbuf_.clear();
        fd_.store(fd);

        if (!handshake(fd, host, port, path)) {
            LOGW("handshake failed");
            fd_.store(-1);
            ::close(fd);
            waitBeforeRetry();
            continue;
        }

        LOGI("connected");
        connected_ = true;

        while (running_.load() && !urlDirty_.load()) {
            bool hasFrame = false;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                    return hasPending_ || !running_.load() || urlDirty_.load();
                });
                if (hasPending_) {
                    // swap 而非拷贝：pending_ 拿回 outBuf_ 的容量，下次 send 就不用再分配
                    outBuf_.clear();
                    pending_.swap(outBuf_);
                    hasPending_ = false;
                    hasFrame = true;
                }
            }
            if (hasFrame && !sendFrame(fd, 0x1, outBuf_)) break;
            if (!pumpIncoming(fd)) break;
        }

        connected_ = false;
        fd_.store(-1);
        ::close(fd);
        LOGW("disconnected");
        waitBeforeRetry();
    }
}

// 重连前等 1 秒，但停止或换地址时立刻返回
void WsClient::waitBeforeRetry() {
    for (int i = 0; i < 10 && running_.load() && !urlDirty_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
