#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// 极简 WebSocket 客户端（仅 ws://，无 TLS）。
// send() 只保留最新一帧：网络跟不上时丢旧帧，绝不阻塞 OpenXR 主循环。
class WsClient {
public:
    WsClient() = default;
    ~WsClient();

    void start(std::string url);
    void stop();
    // 拷贝进已有容量的 pending_，稳态下不再分配
    void send(const std::string &text);

    // 换服务器地址：只交给工作线程，不 join，因而不会阻塞渲染线程
    void setUrl(std::string url);

    // 回调在网络线程上执行，实现方需自行加锁
    void setOnText(std::function<void(const std::string &)> cb) { onText_ = std::move(cb); }

    bool connected() const { return connected_.load(std::memory_order_relaxed); }

private:
    void run();
    void breakSocket();
    void waitBeforeRetry();
    bool handshake(int fd, const std::string &host, const std::string &port, const std::string &path);
    bool writeAll(int fd, const void *data, size_t len) const;
    bool sendFrame(int fd, uint8_t opcode, const std::string &payload);
    bool pumpIncoming(int fd);

    std::string url_;
    std::mutex urlMtx_;
    std::atomic<bool> urlDirty_{false};
    std::atomic<int> fd_{-1};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::mutex mtx_;
    std::condition_variable cv_;
    std::string pending_;
    std::string outBuf_;
    std::string frameBuf_;
    bool hasPending_ = false;

    std::string rbuf_;
    uint32_t rngState_ = 0x2545F491u;
    std::function<void(const std::string &)> onText_;
};
