// 临时编译检查：验证 main.cpp 新增的 T3 辅助函数逻辑/语法
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdio>
#include "auth/t3auth.h"

static std::atomic<bool> g_t3Verified{false};
static std::atomic<bool> g_t3LoggingIn{false};
static std::string       g_t3Card;
static std::string       g_t3Statecode;
static std::mutex        g_t3MsgMutex;
static std::string       g_t3Message;
static char              g_t3InputBuf[128] = {0};

static void t3auth_set_message(const std::string& m) {
    std::lock_guard<std::mutex> lk(g_t3MsgMutex);
    g_t3Message = m;
}
static std::string t3auth_get_message() {
    std::lock_guard<std::mutex> lk(g_t3MsgMutex);
    return g_t3Message;
}
static bool t3auth_try_login(const std::string& card, std::string& statecodeOut, std::string& errOut) {
    std::string imei = t3auth_machine_code();
    printf("[T3验证] 正在验证卡密，机器码=%s ...\n", imei.c_str());
    fflush(stdout);
    T3AuthResult r = t3auth_login(card, imei);
    if (!r.ok) {
        errOut = r.error.empty() ? "未知错误" : r.error;
        printf("[T3验证] 卡密验证失败: %s\n", errOut.c_str());
        fflush(stdout);
        return false;
    }
    statecodeOut = r.statecode;
    printf("[T3验证] 卡密验证通过\n");
    fflush(stdout);
    return true;
}
static void t3auth_do_login_async(const std::string& card) {
    if (g_t3LoggingIn.exchange(true)) return;
    t3auth_set_message("正在验证卡密...");
    std::thread([card]() {
        std::string statecode, err;
        if (t3auth_try_login(card, statecode, err)) {
            g_t3Card = card;
            g_t3Statecode = statecode;
            g_t3Verified.store(true);
            t3auth_set_message("验证通过，推理功能已启用");
            printf("[T3验证] 悬浮窗登录成功\n");
        } else {
            t3auth_set_message("验证失败: " + err);
        }
        g_t3LoggingIn.store(false);
    }).detach();
}
static std::string t3auth_read_clipboard() {
    FILE* p = popen("cmd clipboard get-primary-clip 2>/dev/null", "r");
    if (!p) return "";
    std::string out;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    if (rc != 0 || out.empty()) return "";
    if (out.find("Denial") != std::string::npos ||
        out.find("denial") != std::string::npos ||
        out.find("Exception") != std::string::npos ||
        out.find("exception") != std::string::npos) return "";
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(out.begin());
    return out;
}

int main() {
    t3auth_set_message("请输入卡密并登录");
    printf("msg=%s\n", t3auth_get_message().c_str());
    std::string c = t3auth_read_clipboard();
    printf("clipboard=%s\n", c.c_str());
    return 0;
}
