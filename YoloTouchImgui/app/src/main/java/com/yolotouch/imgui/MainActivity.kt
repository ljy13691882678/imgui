package com.yolotouch.imgui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    companion object {
        private const val REQ_PROJECTION = 1001
        private const val REQ_NOTIF = 1002
    }

    private lateinit var statusText: TextView
    private lateinit var mediaProjectionManager: MediaProjectionManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.status_text)
        mediaProjectionManager =
            getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager

        findViewById<Button>(R.id.start_btn).setOnClickListener {
            requestNotificationPermissionIfNeeded()
            startProjection()
        }
        findViewById<Button>(R.id.stop_btn).setOnClickListener {
            stopService(Intent(this, CaptureService::class.java))
            statusText.setText(R.string.status_idle)
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
