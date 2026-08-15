package com.example.imgui_overlay

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import java.io.File

/**
 * 纯 C/C++ ImGui 悬浮窗的 root 启动器。
 *
 * 原生可执行文件打包在 assets/bin/imgui_overlay，通过 su 部署到
 * /data/local/tmp 并以后台进程方式运行。悬浮窗本身由原生层通过
 * libgui 私有 API 直接创建，无需任何 Java 窗口。
 */
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var logText: TextView

    private val nativeAsset = "bin/imgui_overlay"
    private val remoteDir = "/data/local/tmp"
    private val remoteBin = "$remoteDir/imgui_overlay"
    private val remotePid = "$remoteDir/imgui.pid"
    private val remoteLog = "$remoteDir/imgui.log"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.status_text)
        logText = findViewById(R.id.log_text)
        val btnStart = findViewById<Button>(R.id.btn_start)
        val btnStop = findViewById<Button>(R.id.btn_stop)

        btnStart.setOnClickListener { startOverlay() }
        btnStop.setOnClickListener { stopOverlay() }

        refreshStatus()
    }

    private fun refreshStatus() {
        val su = findSu()
        statusText.text = if (su != null) "检测到 root（$su）" else "未检测到 root！此工具需要 root 权限"
        appendLog("root 状态：${if (su != null) su else "未找到 su"}")
    }

    private fun appendLog(line: String) {
        logText.append("$line\n")
        val scroll = logText.layout
        if (scroll != null) {
            val last = logText.lineCount - 1
            if (last >= 0) logText.scrollTo(0, scroll.getLineTop(last))
        }
    }

    private fun startOverlay() {
        val su = findSu()
        if (su == null) {
            statusText.text = "未检测到 root"
            return
        }
        refreshStatus()

        // 1. 从 APK assets 提取原生二进制到应用私有目录
        val localBin = File(filesDir, "imgui_overlay")
        try {
            assets.open(nativeAsset).use { input ->
                localBin.outputStream().use { output -> input.copyTo(output) }
            }
            localBin.setExecutable(true, true)
            appendLog("已提取原生二进制到 ${localBin.absolutePath}")
        } catch (e: Exception) {
            statusText.text = "提取原生二进制失败：${e.message}"
            appendLog("提取失败：${e.message}")
            return
        }

        // 2. 通过 su 部署到 /data/local/tmp 并后台运行
        //    setsid 让进程完全脱离 su 会话，避免 su 退出后后台进程被系统回收
        val shell = StringBuilder()
        shell.append("cp '$localBin' '$remoteBin'")
        shell.append(" && chmod 755 '$remoteBin'")
        shell.append(" && cd '$remoteDir'")
        shell.append(" && setsid nohup ./imgui_overlay < /dev/null > '$remoteLog' 2>&1 &")
        shell.append(" echo \$! > '$remotePid'")
        shell.append(" && echo '进程已启动，pid: ' ; cat '$remotePid'")

        val output = runAsRoot(su, shell.toString())
        appendLog("su 输出：${output.trim()}")
        statusText.text = "已尝试启动悬浮窗，请查看日志"
    }

    private fun stopOverlay() {
        val su = findSu()
        if (su == null) {
            statusText.text = "未检测到 root"
            return
        }
        val output = runAsRoot(
            su,
            "if [ -f '$remotePid' ]; then kill \$(cat '$remotePid') 2>/dev/null; rm -f '$remotePid'; echo '已停止'; else echo '没有运行中的进程'; fi"
        )
        appendLog("停止输出：${output.trim()}")
        statusText.text = output.trim()
    }

    private fun runAsRoot(su: String, cmd: String): String {
        return try {
            val process = ProcessBuilder(su, "-c", cmd)
                .redirectErrorStream(true)
                .start()
            process.inputStream.bufferedReader().readText().trim()
        } catch (e: Exception) {
            "执行失败：${e.message}"
        }
    }

    /**
     * 找到可用的 su 并实际验证其能提权（su -c id 返回 uid=0）。
     * 仅检查文件存在不可靠（KernelSU / Magisk 挂载方式不同），
     * 因此对每个候选路径都真实执行一次提权测试。
     */
    private fun findSu(): String? {
        val candidates = mutableListOf(
            "/data/adb/magisk/bin/su",
            "/data/adb/ksu/bin/ksu",
            "/data/adb/ksu/bin/su",
            "/sbin/su",
            "/su/bin/su",
            "/system/bin/su",
            "/system/xbin/su",
            "/system/bin/.ext/.su",
            "/vendor/bin/su"
        )
        // 追加 PATH 中能找到的 su
        try {
            val p = ProcessBuilder("which", "su").redirectErrorStream(true).start()
            val out = p.inputStream.bufferedReader().readText().trim()
            if (out.isNotBlank()) candidates += out.lines()
        } catch (_: Exception) {
        }
        // 去重
        val seen = HashSet<String>()
        val unique = candidates.filter { seen.add(it) }

        for (c in unique) {
            if (!File(c).exists()) continue
            val out = runAsRoot(c, "id")
            if (out.contains("uid=0") || out.contains("uid=0(")) {
                return c
            }
        }
        return null
    }
}