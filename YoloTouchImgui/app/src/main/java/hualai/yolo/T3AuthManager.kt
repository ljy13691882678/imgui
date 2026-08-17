package hualai.yolo

import android.content.Context
import android.util.Log

/**
 * T3 网络验证 - APK 侧封装
 *
 * 职责：
 * 1. 卡密登录（网络 I/O，需在后台线程调用）
 * 2. 卡密 + statecode 持久化（SharedPreferences），登录成功后免重复输入
 * 3. 心跳保活：登录成功后由 CaptureService 启动，每 60s 一次；
 *    连续失败 5 次即视为卡密失效/被禁用，强杀 native 进程（与 native 侧行为一致）
 *
 * 与 native 侧（t3auth.cpp）使用同一套配置，验证的是同一张卡密。
 */
object T3AuthManager {
    private const val TAG = "T3AuthManager"
    private const val PREF_NAME = "t3_verify"
    private const val KEY_CARD = "saved_card"
    private const val KEY_STATECODE = "saved_statecode"

    private val heartbeatLock = Any()
    private var heartbeatRunning = false

    // ─── 持久化 ───────────────────────────────────────────────
    fun saveAuth(context: Context, card: String, statecode: String) {
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_CARD, card)
            .putString(KEY_STATECODE, statecode)
            .apply()
    }

    fun loadCard(context: Context): String =
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .getString(KEY_CARD, "") ?: ""

    fun loadStatecode(context: Context): String =
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .getString(KEY_STATECODE, "") ?: ""

    /** 是否已完成过登录验证（保存了有效的 statecode） */
    fun isVerified(context: Context): Boolean = loadStatecode(context).isNotEmpty()

    // ─── 登录 ─────────────────────────────────────────────────
    /**
     * 卡密登录验证。阻塞式网络调用，必须放在后台线程执行。
     * @return 登录结果；可通过 result.success 判断是否成功
     */
    fun login(card: String): T3Verify.T3LoginResult {
        val result = T3Verify.T3LoginResult()
        try {
            val verify = T3Verify()
            verify.initRsa(
                T3Config.LOGIN_CODE, T3Config.NOTICE_CODE, T3Config.VERSION_CODE,
                T3Config.HEARTBEAT_CODE, T3Config.APPKEY, T3Config.RSA_PUBLIC_KEY
            )
            val machineCode = T3Verify.getMachineCode()
            Log.d(TAG, "login start, machine=$machineCode")
            return verify.login(card.trim(), machineCode)
        } catch (e: Exception) {
            Log.e(TAG, "login exception: ${e.message}")
            result.success = false
            result.error = e.message ?: "未知错误"
            return result
        }
    }

    // ─── 心跳保活 ─────────────────────────────────────────────
    /**
     * 启动心跳线程（幂等，只启动一次）。
     * 使用已保存的卡密 + statecode，每 60s 心跳一次；
     * 连续失败 MAX_HEARTBEAT_FAIL 次，强杀 native 进程并停止心跳。
     * 返回是否成功启动（无有效凭据返回 false）。
     */
    fun startHeartbeat(context: Context): Boolean {
        val card = loadCard(context)
        val statecode = loadStatecode(context)
        if (card.isEmpty() || statecode.isEmpty()) {
            Log.w(TAG, "heartbeat skipped: no saved card/statecode")
            return false
        }
        synchronized(heartbeatLock) {
            if (heartbeatRunning) return true
            heartbeatRunning = true
        }
        Thread {
            var failCount = 0
            val verify = T3Verify()
            try {
                verify.initRsa(
                    T3Config.LOGIN_CODE, T3Config.NOTICE_CODE, T3Config.VERSION_CODE,
                    T3Config.HEARTBEAT_CODE, T3Config.APPKEY, T3Config.RSA_PUBLIC_KEY
                )
            } catch (e: Exception) {
                Log.e(TAG, "heartbeat init failed: ${e.message}")
                return@Thread
            }
            while (true) {
                try {
                    Thread.sleep(T3Config.HEARTBEAT_INTERVAL_MS)
                } catch (_: InterruptedException) {
                    break
                }
                val r = verify.heartbeat(card, statecode)
                if (r.success) {
                    failCount = 0
                    Log.d(TAG, "heartbeat ok")
                } else {
                    failCount++
                    Log.e(TAG, "heartbeat fail ($failCount/${T3Config.MAX_HEARTBEAT_FAIL}): ${r.error}")
                    if (failCount >= T3Config.MAX_HEARTBEAT_FAIL) {
                        Log.e(TAG, "heartbeat failed too many times, killing native")
                        RootHelper.killByPattern("imgui ")
                        break
                    }
                }
            }
        }.apply { isDaemon = true; start() }
        Log.d(TAG, "heartbeat thread started")
        return true
    }

    /** 停止心跳（服务停止时调用；线程会在下一轮 sleep 结束后退出） */
    fun stopHeartbeat() {
        synchronized(heartbeatLock) {
            heartbeatRunning = false
        }
    }
}
