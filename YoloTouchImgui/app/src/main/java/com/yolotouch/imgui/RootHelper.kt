package com.yolotouch.imgui

import android.util.Log
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Root 操作封装（KernelSU / Magisk 通用）。
 *
 * Android 架构限制：APK 应用进程自身永远无法获得 root（由 Zygote fork、
 * 运行在 untrusted_app SELinux 域）。root 只能通过 `su` 启动 root 子进程获得。
 * 本模块集中封装 APK 需要的全部 root 操作，供 CaptureService 调用。
 */
object RootHelper {
    private const val TAG = "RootHelper"

    /** su 命令执行结果 */
    data class ShellResult(val exitCode: Int, val output: String, val error: String) {
        val success: Boolean get() = exitCode == 0
    }

    /** 常见 su 路径（KernelSU/Magisk/原生 root 均提供） */
    private val SU_PATHS = listOf(
        "/system/bin/su", "/system/xbin/su",
        "/sbin/su", "/vendor/bin/su", "/su/bin/su",
        "/data/adb/ksu/bin/ksud"
    )

    /** 设备是否存在 root 环境（su 二进制存在） */
    fun isAvailable(): Boolean = SU_PATHS.any { File(it).exists() }

    /** 是否已获得 root 授权（uid=0 即已授权）。KernelSU 未授权时 su 会拒绝 */
    fun hasRootAccess(): Boolean {
        val r = exec("id")
        return r.success && r.output.contains("uid=0")
    }

    /** 同步执行一条 root 命令，超时自动终止，避免授权弹窗/卡死挂起 */
    fun exec(command: String, timeoutSec: Long = 10): ShellResult {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", command))
            // 必须先 waitFor（带超时）再读输出：
            // 若先 readText()，遇到 KernelSU 授权弹窗未确认时进程不退出，
            // readText() 会无限阻塞，导致后续帧循环永远不启动（FPS=0）。
            val finished = p.waitFor(timeoutSec, TimeUnit.SECONDS)
            if (!finished) {
                p.destroy()
                return ShellResult(-1, "", "exec timeout ${timeoutSec}s: $command")
            }
            val out = p.inputStream.bufferedReader().readText()
            val err = p.errorStream.bufferedReader().readText()
            ShellResult(p.exitValue(), out, err)
        } catch (e: Exception) {
            Log.e(TAG, "exec failed: $command -> ${e.message}")
            ShellResult(-1, "", e.message ?: "unknown")
        }
    }

    /** 以 root 后台启动可执行程序（nohup + &），适用于拉起 imgui */
    fun launchBackground(workDir: String, env: Map<String, String>,
                         executable: String, vararg args: String): ShellResult {
        val envStr = env.entries.joinToString(" && ") { "export ${it.key}=${it.value}" }
        val argStr = args.joinToString(" ")
        val cmd = "cd $workDir && $envStr && " +
            "nohup $executable $argStr > /data/local/tmp/imgui.log 2>&1 &"
        return exec(cmd, timeoutSec = 5)
    }

    /** 以 root 终止进程（按命令行关键字匹配） */
    fun killByPattern(pattern: String): ShellResult =
        exec("pkill -f '$pattern'", timeoutSec = 5)
}
