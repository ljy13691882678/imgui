package com.yolotouch.imgui

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * 前台服务：
 *   1. MediaProjection 录屏 → ImageReader 获取 RGBA 帧
 *   2. 帧数据写入共享内存文件（frame.bin，双缓冲，头部与 imgui 侧 ShmFrameHeader 严格一致）
 *   3. 从 assets 解压 imgui 可执行文件 + .so + 模型
 *   4. 通过 su 以 root 拉起 imgui 进程（悬浮窗 + 推理 + 自瞄 + uinput 注入）
 */
class CaptureService : Service() {

    companion object {
        private const val TAG = "CaptureService"
        private const val CHANNEL_ID = "yolo_touch_channel"
        private const val NOTIF_ID = 1

        // 与 C++ ShmFrameHeader 一致
        private const val SHM_MAGIC = 0xA1B2C3D4u
        private const val SHM_HEADER_SIZE = 64
        private const val SHM_BUFFER_COUNT = 2

        private const val ASSET_NATIVE_DIR = "native"
        private const val ASSET_IMGUI = "imgui"
        private const val ASSET_MODEL = "valorant_256_v26n.tflite"

        fun startWithProjection(context: Context, resultCode: Int, data: Intent) {
            val intent = Intent(context, CaptureService::class.java)
            intent.putExtra("resultCode", resultCode)
            intent.putExtra("resultData", data)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val ioExecutor: ExecutorService = Executors.newSingleThreadExecutor()

    private lateinit var mediaProjectionManager: MediaProjectionManager
    private var mediaProjection: MediaProjection? = null
    private var imageReader: ImageReader? = null
    private var virtualDisplay: VirtualDisplay? = null

    private val running = AtomicBoolean(true)
    private val lastSeq = AtomicLong(0)

    // 防止写帧任务在 ioExecutor 上堆积（一次只处理一帧，多余帧交给下次 acquireLatestImage）
    private val frameWriting = AtomicBoolean(false)

    private var shmFile: File? = null
    private var captureW = 0
    private var captureH = 0
    private var rotation = 0

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        mediaProjectionManager =
            getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        if (intent == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        val resultCode = intent.getIntExtra("resultCode", 0)
        val data = intent.getParcelableExtra<Intent>("resultData")
        startProjection(resultCode, data)
        return START_STICKY
    }

    private fun startForegroundCompat() {
        val pi = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        val notif: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("YoloTouch 自瞄")
            .setContentText("录屏中，悬浮窗运行中")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    // ─── 录屏 ────────────────────────────────────────────────
    private fun startProjection(resultCode: Int, data: Intent?) {
        if (data == null) {
            Log.e(TAG, "no projection data")
            stopSelf()
            return
        }
        val projection = mediaProjectionManager.getMediaProjection(resultCode, data)
        if (projection == null) {
            Log.e(TAG, "getMediaProjection failed")
            stopSelf()
            return
        }
        mediaProjection = projection
        projection.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                Log.d(TAG, "MediaProjection stopped")
                stopAll()
            }
        }, mainHandler)

        val metrics = DisplayMetrics()
        (getSystemService(Context.WINDOW_SERVICE) as WindowManager)
            .defaultDisplay.getRealMetrics(metrics)
        captureW = metrics.widthPixels
        captureH = metrics.heightPixels
        rotation = try {
            (getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.rotation
        } catch (e: Exception) {
            0
        }
        val density = metrics.densityDpi
        Log.d(TAG, "screen=${captureW}x${captureH} rotation=$rotation")

        // MediaProjection 的 ImageReader 必须注册 OnImageAvailableListener，
        // 否则部分设备/系统上 producer 不持续产帧，acquireLatestImage() 一直返回 null，
        // 导致共享内存无新帧、imgui 推理 FPS 显示为 0。
        imageReader = ImageReader.newInstance(captureW, captureH, PixelFormat.RGBA_8888, 2)
            .also { it.setOnImageAvailableListener({ reader -> onFrameAvailable(reader) }, mainHandler) }
        virtualDisplay = projection.createVirtualDisplay(
            "YoloTouchCapture",
            captureW, captureH, density,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            imageReader!!.surface, null, null
        )
        if (virtualDisplay == null) {
            Log.e(TAG, "createVirtualDisplay failed")
            stopSelf()
            return
        }
        Log.d(TAG, "virtualDisplay created, capture started")

        ioExecutor.execute {
            try {
                if (!prepareNativeAndRun()) {
                    Log.e(TAG, "prepare native failed")
                    mainHandler.post { stopAll() }
                }
            } catch (e: Exception) {
                Log.e(TAG, "setup error", e)
                mainHandler.post { stopAll() }
            }
        }
    }

    // ─── 解压原生文件 + 拉起 imgui ──────────────────────────
    private fun prepareNativeAndRun(): Boolean {
        // 0. 检查 root 环境与授权（KernelSU/Magisk）
        if (!RootHelper.isAvailable()) {
            Log.e(TAG, "未检测到 su，请确认已安装 KernelSU/Magisk 并授予本应用 root 权限")
            return false
        }
        if (!RootHelper.hasRootAccess()) {
            Log.e(TAG, "尚未获得 root 授权，请在 KernelSU/Magisk 中允许本应用获取 root 权限")
            return false
        }
        Log.d(TAG, "root 环境正常，已获得 root 授权")

        val dir = File(filesDir, "native").apply { mkdirs() }
        val binDir = File(filesDir, "bin").apply { mkdirs() }

        // 1. 解压 .so 动态库
        val libDir = File(dir, "lib").apply { mkdirs() }
        val soNames = assets.list("native/lib") ?: emptyArray()
        for (name in soNames) {
            if (!name.endsWith(".so")) continue
            val out = File(libDir, name)
            if (!out.exists() || out.length() == 0L) {
                assets.open("native/lib/$name").use { input ->
                    FileOutputStream(out).use { output -> input.copyTo(output) }
                }
            }
            out.setReadable(true, false)
            out.setExecutable(true, false)
        }

        // 2. 解压 imgui 可执行文件
        val imgui = File(binDir, ASSET_IMGUI)
        if (!imgui.exists() || imgui.length() == 0L) {
            assets.open("$ASSET_NATIVE_DIR/$ASSET_IMGUI").use { input ->
                FileOutputStream(imgui).use { output -> input.copyTo(output) }
            }
        }
        imgui.setExecutable(true, true)

        // 3. 解压全部模型到 models/ 目录（面板可按需切换）
        val modelDir = File(dir, "models").apply { mkdirs() }
        val modelNames = assets.list("$ASSET_NATIVE_DIR/models") ?: emptyArray()
        for (name in modelNames) {
            if (!name.endsWith(".tflite")) continue
            val out = File(modelDir, name)
            if (!out.exists() || out.length() == 0L) {
                assets.open("$ASSET_NATIVE_DIR/models/$name").use { input ->
                    FileOutputStream(out).use { output -> input.copyTo(output) }
                }
            }
            out.setReadable(true, false)
        }
        // 默认模型（确保存在，imgui 用它初始化）
        val model = File(modelDir, ASSET_MODEL)
        if (!model.exists()) {
            assets.open("$ASSET_NATIVE_DIR/models/$ASSET_MODEL").use { input ->
                FileOutputStream(model).use { output -> input.copyTo(output) }
            }
        }

        // 4. 创建共享内存文件（头部 + 双缓冲）
        val shm = File(filesDir, "frame.bin")
        initShm(shm)
        shm.setReadable(true, false)
        shm.setWritable(true, false)

        // 5. 以 root 拉起 imgui（使用 RootHelper 封装）
        val ldPath = libDir.absolutePath
        val result = RootHelper.launchBackground(
            workDir = binDir.absolutePath,
            env = mapOf("LD_LIBRARY_PATH" to ldPath),
            executable = imgui.absolutePath,
            model.absolutePath, shm.absolutePath, binDir.absolutePath
        )
        Log.d(TAG, "root launch: rc=${result.exitCode} out=${result.output} err=${result.error}")

        // 帧写入由 ImageReader 的 OnImageAvailableListener 驱动（见 onFrameAvailable）
        return true
    }

    private fun initShm(file: File) {
        try {
            if (file.exists()) file.delete()
        } catch (_: Exception) {}
        val sizePerFrame = captureW * captureH * 4
        val total = SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(total.toLong())
            val hdr = ByteBuffer.allocate(SHM_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            hdr.putInt(SHM_MAGIC.toInt())
            hdr.putInt(captureW)
            hdr.putInt(captureH)
            hdr.putInt(sizePerFrame) // rowStride（RGBA 无对齐）
            hdr.putInt(4)            // pixelStride
            hdr.putInt(rotation)
            hdr.putInt(sizePerFrame) // sizePerFrame
            hdr.putInt(0)            // lastSeq
            hdr.put(ByteArray(32))   // reserved
            raf.seek(0)
            raf.write(hdr.array())
        }
        shmFile = file
        Log.d(TAG, "shm initialized size=$total")
    }

    // ─── 写帧（由 OnImageAvailableListener 驱动） ─────────────
    private fun onFrameAvailable(reader: ImageReader) {
        if (!running.get()) return
        // 上一帧还在写（ioExecutor 单线程排队），则跳过本次，
        // 下一帧到达时 acquireLatestImage() 会取到最新一帧，避免堆积。
        if (!frameWriting.compareAndSet(false, true)) return
        ioExecutor.execute {
            try {
                writeFrame(reader)
            } finally {
                frameWriting.set(false)
            }
        }
    }

    private fun writeFrame(reader: ImageReader) {
        val shm = shmFile ?: return
        val image = reader.acquireLatestImage() ?: return
        try {
            val plane = image.planes[0]
            val buf = plane.buffer
            val w = image.width
            val h = image.height
            val rowStride = plane.rowStride
            val sizePerFrame = rowStride * h

            if (shm.length() < (SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT).toLong()) {
                RandomAccessFile(shm, "rw").use { it.setLength(
                    (SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT).toLong()
                ) }
            }

            val seq = lastSeq.incrementAndGet()
            val bufIdx = ((seq % SHM_BUFFER_COUNT).toInt()) * sizePerFrame
            val offset = SHM_HEADER_SIZE + bufIdx

            RandomAccessFile(shm, "rw").use { raf ->
                // 写入帧数据
                val pos = buf.position()
                buf.rewind()
                if (buf.remaining() >= sizePerFrame) {
                    val chunk = ByteArray(sizePerFrame)
                    buf.get(chunk)
                    raf.seek(offset.toLong())
                    raf.write(chunk)
                }
                buf.position(pos)

                // 更新 header（宽度/高度/stride + 序号）
                raf.seek(0)
                val hdr = ByteBuffer.allocate(SHM_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
                hdr.putInt(SHM_MAGIC.toInt())
                hdr.putInt(w)
                hdr.putInt(h)
                hdr.putInt(rowStride)
                hdr.putInt(plane.pixelStride)
                hdr.putInt(rotation)
                hdr.putInt(sizePerFrame)
                hdr.putInt(seq.toInt())
                hdr.put(ByteArray(32))
                raf.write(hdr.array())
            }
            if (seq % 60 == 0L) Log.d(TAG, "captured seq=$seq")
        } catch (e: Exception) {
            Log.e(TAG, "frame write error: ${e.message}")
        } finally {
            image.close()
        }
    }

    // ─── 通知/清理 ───────────────────────────────────────────
    private fun createNotificationChannel() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID, "YoloTouch 自瞄服务", NotificationManager.IMPORTANCE_LOW
        )
        nm.createNotificationChannel(channel)
    }

    private fun stopAll() {
        running.set(false)
        try {
            virtualDisplay?.release()
        } catch (_: Exception) {}
        try {
            imageReader?.close()
        } catch (_: Exception) {}
        try {
            mediaProjection?.stop()
        } catch (_: Exception) {}
        // 停止 imgui 进程
        RootHelper.killByPattern("imgui ")
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        stopAll()
        ioExecutor.shutdownNow()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
