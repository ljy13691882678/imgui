package hualai.yolo

/**
 * T3 网络验证 - APK 侧配置
 * 官网: https://www.t3yanzheng.com
 *
 * 注意：必须与 native 侧（imgui_overlay/src/auth/t3auth.cpp）使用同一套
 * 调用码 + APPKEY + RSA 公钥，保证 APK 与二进制验证的是同一张卡密。
 * 后台需开启 RSA 算法（HEX 编码 + 时间戳 + 双向签名 + JSON 返回）。
 */
object T3Config {
    // 本地版本号（与服务端版本号对比，未使用可忽略）
    const val LOCAL_VERSION = "1000"

    // 单码登录调用码
    const val LOGIN_CODE = "364BB9C878698A91"
    // 获取程序公告调用码
    const val NOTICE_CODE = "4AD6AF47E16C2641"
    // 获取程序最新版本号调用码
    const val VERSION_CODE = "6DEFFB0C83B0113C"
    // 单码卡密心跳验证调用码
    const val HEARTBEAT_CODE = "6653C56842BB614E"
    // 程序密钥 APPKEY
    const val APPKEY = "7b16d44d7af6f7762052a2ebe3020584"

    // RSA 公钥（与 native 侧一致）
    const val RSA_PUBLIC_KEY =
        "-----BEGIN PUBLIC KEY-----\n" +
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCW0id/+Q1XLTATn6vnSNVHYSd6\n" +
        "c+LNukHq3xy+Boa6wX9Mj7fA0++4UyHrYavwLWqYd6i6dpd4Udb4JNoynwJ6gDVb\n" +
        "lENn1r1dFhORWqjBHTY2zEjK76IJU8GS1u6NFTWJsvB9Db5paTfIjfSJHRcbeX25\n" +
        "P2M3PQfwMNqIR8AwYwIDAQAB\n" +
        "-----END PUBLIC KEY-----"

    // 心跳间隔（毫秒）与最大连续失败次数（与 native 侧一致）
    const val HEARTBEAT_INTERVAL_MS = 60 * 1000L
    const val MAX_HEARTBEAT_FAIL = 5
}
