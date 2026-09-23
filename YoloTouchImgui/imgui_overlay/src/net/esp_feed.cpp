#include "net/esp_feed.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace EspFeed {
namespace {

std::mutex        gMutex;              // 保护下面这些
std::vector<Box>  gBoxes;
uint64_t          gFrameId = 0;
long long         gLastFrameMs = 0;    // 收到最后一帧的时间(steady clock ms)
long long         gLastByteMs = 0;     // 收到最后一个字节的时间
long              gFrames = 0;
long              gBytes = 0;
bool              gConnected = false;
std::string       gMessage = "未启动";

std::string       gHost = "192.168.137.1";
int               gPort = 27015;

std::atomic<bool> gRun{false};
std::thread       gThread;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 拆一行: "A 0 0.99 0.1 0.2 0.3 0.4 12.5 名字"
bool parseActorLine(const std::string& line, Box& out) {
    int cls = 0;
    float score = 1.0f, x1 = 0, y1 = 0, x2 = 0, y2 = 0, dist = -1.0f;
    int consumed = 0;
    int n = sscanf(line.c_str(), "A %d %f %f %f %f %f %f %n",
                   &cls, &score, &x1, &y1, &x2, &y2, &dist, &consumed);
    if (n < 7) return false;
    out.cls = cls;
    out.score = score;
    out.x1 = x1; out.y1 = y1; out.x2 = x2; out.y2 = y2;
    out.distM = dist;
    // 名字: 第 7 个空格之后的全部内容(允许空格/中文)
    out.name.clear();
    if (consumed > 0 && consumed < (int)line.size())
        out.name = line.substr(consumed);
    // 去掉行尾 \r
    while (!out.name.empty() && (out.name.back() == '\r' || out.name.back() == '\n'))
        out.name.pop_back();
    if (out.name.size() > 40) out.name.resize(40);
    return true;
}

void worker() {
    std::string host;
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(gMutex);
        host = gHost;
        port = gPort;
    }
    while (gRun.load()) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            // 允许填域名
            hostent* he = gethostbyname(host.c_str());
            if (!he) {
                std::lock_guard<std::mutex> lk(gMutex);
                gMessage = "地址无效: " + host;
                ::close(fd);
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            memcpy(&addr.sin_addr, he->h_addr, sizeof(addr.sin_addr));
        }

        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            {
                std::lock_guard<std::mutex> lk(gMutex);
                gConnected = false;
                gMessage = "连不上 PC " + host + ":" + std::to_string(port);
            }
            ::close(fd);
            for (int i = 0; i < 20 && gRun.load(); ++i)      // 2 秒后重试
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(gMutex);
            gConnected = true;
            gMessage = "已连接 " + host + ":" + std::to_string(port);
            gLastByteMs = nowMs();
        }
        printf("[esp] 已连接 PC %s:%d\n", host.c_str(), port);
        fflush(stdout);

        std::string buf;
        std::vector<Box> pending;
        int expect = -1;
        char tmp[8192];
        while (gRun.load()) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            {
                std::lock_guard<std::mutex> lk(gMutex);
                gBytes += n;
                gLastByteMs = nowMs();
            }
            buf.append(tmp, (size_t)n);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (line.rfind("ESP1 ", 0) == 0) {
                    int cnt = 0;
                    if (sscanf(line.c_str(), "ESP1 %d", &cnt) == 1) {
                        expect = cnt;
                        pending.clear();
                        pending.reserve((size_t)std::max(0, cnt));
                        if (cnt == 0) {           // 空帧: 立即清空画面
                            std::lock_guard<std::mutex> lk(gMutex);
                            gBoxes.clear();
                            ++gFrameId;
                            gFrames++;
                            gLastFrameMs = nowMs();
                        }
                    }
                } else if (line[0] == 'A') {
                    Box b;
                    if (parseActorLine(line, b)) pending.push_back(std::move(b));
                    if (expect >= 0 && (int)pending.size() >= expect) {
                        std::lock_guard<std::mutex> lk(gMutex);
                        gBoxes.swap(pending);
                        ++gFrameId;
                        gFrames++;
                        gLastFrameMs = nowMs();
                        pending.clear();
                        expect = -1;
                    }
                }
            }
            if (buf.size() > 65536) buf.clear();   // 异常情况下别把内存吃满
        }

        ::close(fd);
        {
            std::lock_guard<std::mutex> lk(gMutex);
            gConnected = false;
            gBoxes.clear();
            gMessage = "连接断开, 重试中…";
        }
        printf("[esp] PC 连接断开\n");
        fflush(stdout);
    }
}

}  // namespace

void Configure(const std::string& host, int port) {
    std::lock_guard<std::mutex> lk(gMutex);
    if (!host.empty()) gHost = host;
    if (port > 0 && port < 65536) gPort = port;
}

void Start() {
    if (gRun.exchange(true)) return;
    gThread = std::thread(worker);
}

void Stop() {
    if (!gRun.exchange(false)) return;
    if (gThread.joinable()) gThread.join();
    std::lock_guard<std::mutex> lk(gMutex);
    gConnected = false;
    gBoxes.clear();
    gMessage = "已停止";
}

bool Running() { return gRun.load(); }

bool GetLatest(std::vector<Box>& out, uint64_t& frameId) {
    std::lock_guard<std::mutex> lk(gMutex);
    out = gBoxes;
    frameId = gFrameId;
    return gConnected && !gBoxes.empty();
}

Stats GetStats() {
    std::lock_guard<std::mutex> lk(gMutex);
    Stats s;
    s.running = gRun.load();
    s.connected = gConnected;
    s.boxes = (int)gBoxes.size();
    s.frames = gFrames;
    s.bytes = gBytes;
    s.lastFrameMs = gLastFrameMs ? (nowMs() - gLastFrameMs) : -1;
    s.host = gHost;
    s.port = gPort;
    s.message = gMessage;
    return s;
}

}  // namespace EspFeed

