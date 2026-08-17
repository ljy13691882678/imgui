package com.yolotouch.imgui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
import java.util.concurrent.Executors

class MainActivity : AppCompatActivity() {

    companion object {
        private const val REQ_PROJECTION = 1001
        private const val REQ_NOTIF = 1002
    }

    private lateinit var statusText: TextView
    private lateinit var verifyStatus: TextView
    private lateinit var cardInput: EditText
    private lateinit var verifyBtn: Button
    private lateinit var startBtn: Button
    private lateinit var mediaProjectionManager: MediaProjectionManager
    private val checkExecutor = Executors.newSingleThreadExecutor()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.status_text)
        verifyStatus = findViewById(R.id.verify_status)
        cardInput = findViewById(R.id.card_input)
        verifyBtn = findViewById(R.id.verify_btn)
        startBtn = findViewById(R.id.start_btn)
        mediaProjectionManager =
            getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager

        // 回填上次保存的卡密
        val savedCard = T3AuthManager.loadCard(this)
        if (savedCard.isNotEmpty()) cardInput.setText(savedCard)

        verifyBtn.setOnClickListener { doVerify() }
        startBtn.setOnClickListener {
            requestNotificationPermissionIfNeeded()
            if (!T3AuthManager.isVerified(this)) {
                verifyStatus.text = getString(R.string.verify_required)
                return@setOnClickListener
            }
            startProjection()
        }
        findViewById<Button>(R.id.stop_btn).setOnClickListener {
            stopService(Intent(this, CaptureService::class.java))
            statusText.setText(R.string.status_idle)
        }

        checkRootStatus()
        showLastStatus()
        refreshVerifyStatus()
    }

    /** 更新验证状态显示（基于本地保存的凭据） */
    private fun refreshVerifyStatus() {
        if (T3AuthManager.isVerified(this)) {
            verifyStatus.setText(R.string.verify_success)
        } else {
            verifyStatus.setText(R.string.verify_pending)
        }
    }

    /** 卡密验证：后台线程登录，成功后保存凭据 */
    private fun doVerify() {
        val card = cardInput.text.toString().trim()
        if (card.isEmpty()) {
            verifyStatus.setText(R.string.verify_pending)
            return
        }
        verifyBtn.isEnabled = false
        verifyStatus.setText(R.string.verify_working)
        checkExecutor.execute {
            val result = T3AuthManager.login(card)
            runOnUiThread {
                verifyBtn.isEnabled = true
                if (result.success) {
                    // 登录成功：保存卡密 + statecode，供后续启动/心跳使用
                    T3AuthManager.saveAuth(this, card, result.statecode)
                    verifyStatus.setText(R.string.verify_success)
                } else {
                    verifyStatus.text = getString(R.string.verify_fail, result.error ?: "未知错误")
                }
            }
        }
    }

    /** 显示上次 CaptureService 写入的运行状态（失败原因在服务退出后仍可见） */
    private fun showLastStatus() {
        try {
            val f = File(filesDir, "yolotouch_status.txt")
            if (f.exists()) {
                val s = f.readText().trim()
                if (s.isNotEmpty()) statusText.text = s
            }
        } catch (_: Exception) {}
    }

    override fun onDestroy() {
        checkExecutor.shutdownNow()
        super.onDestroy()
    }

    /** 后台检查 root 环境与授权状态（su 调用可能触发 KernelSU 弹窗，勿在 UI 线程） */
    private fun checkRootStatus() {
        checkExecutor.execute {
            val available = RootHelper.isAvailable()
            val granted = if (available) RootHelper.hasRootAccess() else false
            runOnUiThread {
                statusText.text = when {
                    !available -> "未检测到 su（请安装 KernelSU/Magisk）"
                    !granted -> "root 未授权：请在 KernelSU/Magisk 中允许本应用，然后重启应用"
                    else -> getString(R.string.status_idle)
                }
            }
        }
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
            ) {
                ActivityCompat.requestPermissions(
                    this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIF
                )
            }
        }
    }

    private fun startProjection() {
        // MediaProjection 需要用户在系统弹窗中确认
        startActivityForResult(
            mediaProjectionManager.createScreenCaptureIntent(), REQ_PROJECTION
        )
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_PROJECTION) return
        if (resultCode != RESULT_OK || data == null) {
            statusText.text = "未获得录屏权限"
            return
        }
        CaptureService.startWithProjection(this, resultCode, data)
        statusText.setText(R.string.status_running)
    }
}
