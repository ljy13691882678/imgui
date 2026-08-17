/**
 * T3 网络验证 - native 侧封装
 * 官网: https://www.t3yanzheng.com
 *
 * 给 imgui 可执行文件提供卡密登录验证 + 心跳保活。
 * 验证逻辑独立于 APK：即使 APK 被破解跳过验证，这里仍会向服务器校验同一张卡密，
 * 验证失败则进程直接退出（悬浮窗起不来）。
 */
#ifndef T3AUTH_H
#define T3AUTH_H

#include <string>

/* 登录结果 */
struct T3AuthResult {
    bool ok = false;
    std::string error;      // 失败原因
    std::string statecode;  // 成功后的 statecode（心跳需要）
};

/* 用卡密 + 机器码登录验证，成功返回 ok=true */
T3AuthResult t3auth_login(const std::string& card, const std::string& imei);

/* 启动心跳线程：每 60s 一次，连续失败 5 次强制 exit(1)（进程退出） */
void t3auth_start_heartbeat(const std::string& card, const std::string& statecode);

/* 获取机器码（无 APK 单独运行时作为 imei 兜底） */
std::string t3auth_machine_code();

#endif /* T3AUTH_H */
