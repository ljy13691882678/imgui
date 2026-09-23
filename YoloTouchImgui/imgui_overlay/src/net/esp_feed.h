#pragma once
//
// esp_feed.h —— PC → 手机 的敌人数据(TCP)接收
//
// 为什么用它: 坐标由 PC 侧(自建 TGCP 网关 + 解析器)解密得到, 手机端不再需要
// 抓 UDP / 读内存 / 跑 YOLO, 只负责"画框 + 自瞄"。
//
// 协议(UTF-8 文本, \n 结尾):
//     ESP1 <count> <selfX> <selfY> <selfZ> <selfYawDeg> <myTeam>
//     A <cls> <score> <x1> <y1> <x2> <y2> <dist_m> <name>
//     ... 共 count 行
//   cls: 0=敌人 1=AI 2=队友 3=物资 4=死亡盒
//   坐标是【相对整屏的归一化 0~1】, 与 YOLO 检测框同坐标系
//
#include <cstdint>
#include <string>
#include <vector>

namespace EspFeed {

struct Box {
    int         cls = 0;
    float       score = 1.0f;
    float       x1 = 0, y1 = 0, x2 = 1, y2 = 1;
    float       distM = -1.0f;
    std::string name;
};

struct Stats {
    bool     running = false;
    bool     connected = false;
    int      boxes = 0;
    long     frames = 0;
    long     bytes = 0;
    long long lastFrameMs = 0;    // 距上一帧多久(ms)
    std::string host;
    int      port = 0;
    std::string message;          // 最近一次错误/状态
};

// 设置目标 PC 地址(改完需要 Stop→Start 生效)
void Configure(const std::string& host, int port);

// 启动/停止后台接收线程; 重复调用安全
void Start();
void Stop();
bool Running();

// 取最新一帧(不消费): 返回是否有数据; frameId 用于判断"是不是新帧"
bool GetLatest(std::vector<Box>& out, uint64_t& frameId);

Stats GetStats();

}  // namespace EspFeed

