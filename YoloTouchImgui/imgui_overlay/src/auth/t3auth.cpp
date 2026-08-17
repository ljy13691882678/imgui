/**
 * T3 网络验证 - native 侧封装实现
 * 基于 T3 C++ SDK（t3sdk.cpp），纯 C++ 无外部依赖。
 */
#include "t3auth.h"

#include "../t3sdk/t3sdk.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>

/* ============================================================
 * T3 平台对接参数（请替换为你在 t3yanzheng.com 后台的程序配置）
 * 注意：必须与 APK 侧（T3Config.kt）使用同一套调用码 + APPKEY + RSA 公钥，
 * 保证两张卡密是同一张。
 * ============================================================ */
static const char* T3_LOGIN_CODE     = "364BB9C878698A91";  /* 单码登录调用码 */
static const char* T3_NOTICE_CODE    = "4AD6AF47E16C2641";  /* 公告调用码 */
static const char* T3_VERSION_CODE   = "6DEFFB0C83B0113C";  /* 版本号调用码 */
static const char* T3_HEARTBEAT_CODE = "6653C56842BB614E";  /* 单码心跳调用码 */
static const char* T3_APPKEY         = "7b16d44d7af6f7762052a2ebe3020584";

static const char* T3_RSA_PUBLIC_KEY =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCW0id/+Q1XLTATn6vnSNVHYSd6\n"
    "c+LNukHq3xy+Boa6wX9Mj7fA0++4UyHrYavwLWqYd6i6dpd4Udb4JNoynwJ6gDVb\n"
    "lENn1r1dFhORWqjBHTY2zEjK76IJU8GS1u6NFTWJsvB9Db5paTfIjfSJHRcbeX25\n"
    "P2M3PQfwMNqIR8AwYwIDAQAB\n"
    "-----END PUBLIC KEY-----";

static T3Verify* createVerify() {
    T3Verify* verify = new T3Verify();
    if (!verify->initRSA(T3_LOGIN_CODE, T3_NOTICE_CODE, T3_VERSION_CODE,
                         T3_HEARTBEAT_CODE, T3_APPKEY, T3_RSA_PUBLIC_KEY)) {
        delete verify;
        return nullptr;
    }
    return verify;
}

T3AuthResult t3auth_login(const std::string& card, const std::string& imei) {
    T3AuthResult res;
    T3Verify* verify = createVerify();
    if (!verify) {
        res.error = "SDK 初始化失败";
        return res;
    }
    T3LoginResult r = verify->login(card, imei);
    delete verify;
    if (!r.success) {
        res.error = r.error;
        return res;
    }
    res.ok = true;
    res.statecode = r.statecode;
    return res;
}

void t3auth_start_heartbeat(const std::string& card, const std::string& statecode) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;  // 只启动一次

    std::thread([card, statecode]() {
        const int INTERVAL_SEC = 60;
        const int MAX_FAIL = 5;

        T3Verify* verify = createVerify();
        if (!verify) {
            // 注意：native 侧 stderr 被 stderr_shim 重定向到 /dev/null，
            // 且 shell 启动时 2>&1 到 imgui.log —— 统一用 stdout 打印日志。
            printf("[T3心跳] SDK 初始化失败\n");
            fflush(stdout);
            return;
        }

        int fail = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(INTERVAL_SEC));
            T3Result hb = verify->heartbeat(card, statecode);
            if (hb.success) {
                fail = 0;
                printf("[T3心跳] 验证成功\n");
            } else {
                fail++;
                printf("[T3心跳] 验证失败 (%d/%d): %s\n",
                       fail, MAX_FAIL, hb.error.c_str());
                if (fail >= MAX_FAIL) {
                    printf("[T3心跳] 连续失败 %d 次，程序强制退出！\n", MAX_FAIL);
                    fflush(stdout);
                    exit(1);
                }
            }
            fflush(stdout);
        }
    }).detach();
}

std::string t3auth_machine_code() {
    return getMachineCode();
}
