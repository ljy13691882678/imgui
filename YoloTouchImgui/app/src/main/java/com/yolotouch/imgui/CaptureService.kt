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
import android.media.Image
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
        private const val SHM_HEADER_SIZE = 80
        private const val SHM_BUFFER_COUNT = 2

        // 居中裁剪可选边长（与 C++ CROP_OPTIONS 一致），默认 416
        private val CROP_OPTIONS = intArrayOf(620, 416, 320, 256)
        private const val CROP_DEFAULT = 416

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
    // setupExecutor：解压/拉起 imgui（耗时长，秒级）
    private val setupExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    // 帧消费线程：独立线程上轮询 acquireLatestImage → 写共享内存 → close。
    // 不依赖 OnImageAvailableListener —— 该回调在部分设备/系统上可能只触发一次或
    // 丢失，导致 ImageReader 缓冲被占满后 producer 停摆，共享内存永远只有 1 帧
    // （推理 FPS=0）。轮询方式总能持续消费，producer 不会被卡死。
    private var frameThread: Thread? = null

    private lateinit var mediaProjectionManager: MediaProjectionManager
    private var mediaProjection: MediaProjection? = null
    private var imageReader: ImageReader? = null
    private var virtualDisplay: VirtualDisplay? = null

    private val running = AtomicBoolean(true)
    private val lastSeq = AtomicLong(0)

    // 写帧诊断计数（写入共享内存头部，imgui 侧可直接读取判断帧源是否在产帧）
    private val writeAttempts = AtomicLong(0)   // writeFrame 被调用次数（取到非空帧）
    private val writeSuccesses = AtomicLong(0)  // 写帧成功次数
    private val acquireNulls = AtomicLong(0)    // acquireLatestImage 返回 null 次数
    private val writeFails = AtomicLong(0)      // 写帧异常次数

    private var shmFile: File? = null
    private var captureW = 0
    private var captureH = 0
    private var rotation = 0
    // 帧线程复用的共享内存文件句柄与裁剪缓冲：避免每帧 open/close 文件、
    // 以及逐行多次 write() 系统调用（推理仅 ~1ms，帧率瓶颈在文件 I/O）。
    private var shmRaf: RandomAccessFile? = null
    private var cropBuffer = ByteArray(620 * 620 * 4)  // 最大裁剪 620×620×RGBA

    // 最近一帧的尺寸/stride（flushDiagnostics 无新帧时写头部用，避免写 0）
    @Volatile private var lastW = 0
    @Volatile private var lastH = 0
    @Volatile private var lastRowStride = 0
    @Volatile private var lastSizePerFrame = 0
    private var lastDiagFlushMs = 0L

    // 居中裁剪状态：只把屏幕中心 crop×crop 的准星区域写入共享内存，
    // 大幅减小写帧体积 → 帧率提升（可到 60-120）。cropRequest 由 C++ 面板
    // 写入头部，这里读取并应用（默认 CROP_DEFAULT）。
    @Volatile private var activeCrop = 416
    @Volatile private var activeCropOffsetX = 0
    @Volatile private var activeCropOffsetY = 0
    private var lastCropCheckMs = 0L

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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, buildNotification("录屏中，悬浮窗运行中"),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIF_ID, buildNotification("录屏中，悬浮窗运行中"))
        }
    }

    private fun buildNotification(text: String): Notification {
        val pi = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("YoloTouch 自瞄")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }

    /** 实时更新前台通知文本（启动进度 / 错误原因，用户可直接看到卡在哪一步） */
    private fun updateNotification(text: String) {
        try {
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .notify(NOTIF_ID, buildNotification(text))
        } catch (_: Exception) {}
    }

    /** 把最新状态持久化，MainActivity 打开时读取显示（服务退出/通知被移除后仍可见） */
    private fun writeStatus(text: String) {
        try {
            File(filesDir, "yolotouch_status.txt").writeText(text)
        } catch (_: Exception) {}
    }

    /**
     * 记录失败并停止服务。
     * 用独立的普通通知（ID=NOTIF_ID+1）+ 状态文件，避免 stopForeground 把
     * 错误信息一起移除，保证用户无需 adb 也能看到失败原因。
     */
    private fun showError(msg: String) {
        diagLog("ERROR: $msg")
        writeStatus("启动失败：$msg")
        try {
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .notify(NOTIF_ID + 1, buildNotification("启动失败：$msg"))
        } catch (_: Exception) {}
        stopAll()
    }

    /** 追加诊断日志到 /data/local/tmp/yolotouch_service.log（root 文件管理器可读） */
    private fun diagLog(msg: String) {
        Log.d(TAG, msg)
        try {
            val line = "[${android.text.format.DateFormat.format("HH:mm:ss", System.currentTimeMillis())}] $msg\n"
            FileOutputStream("/data/local/tmp/yolotouch_service.log", true).use {
                it.write(line.toByteArray())
            }
        } catch (_: Exception) {}
    }

    // 小端读取 int（共享内存头部为 little-endian）。
    // 注意：RandomAccessFile.readInt() 按 Java DataInput 大端读，直接用于
    // C++ 写入的字段会把 320 读成 0x40010000，导致裁剪切换永远不生效。
    private fun readIntLE(raf: RandomAccessFile): Int {
        val b = ByteArray(4)
        raf.readFully(b)
        return (b[0].toInt() and 0xFF) or
            ((b[1].toInt() and 0xFF) shl 8) or
            ((b[2].toInt() and 0xFF) shl 16) or
            ((b[3].toInt() and 0xFF) shl 24)
    }

    // ─── 录屏 ────────────────────────────────────────────────
    private fun startProjection(resultCode: Int, data: Intent?) {
        updateNotification("正在启动录屏...")
        diagLog("startProjection begin")
        try {
            if (data == null) {
                showError("无录屏授权数据，请重新点击开始")
                return
            }
            val projection = mediaProjectionManager.getMediaProjection(resultCode, data)
            if (projection == null) {
                showError("getMediaProjection=null，请重新授权录屏")
                return
            }
            mediaProjection = projection
            projection.registerCallback(object : MediaProjection.Callback() {
                override fun onStop() {
                    Log.d(TAG, "MediaProjection stopped")
                    stopAll()
                }
            }, mainHandler)

            val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val bounds = wm.currentWindowMetrics.bounds
                captureW = bounds.width()
                captureH = bounds.height()
            } else {
                val metrics = DisplayMetrics()
                wm.defaultDisplay.getRealMetrics(metrics)
                captureW = metrics.widthPixels
                captureH = metrics.heightPixels
            }
            rotation = try {
                wm.defaultDisplay.rotation
            } catch (e: Exception) {
                0
            }
            if (captureW <= 0 || captureH <= 0) {
                showError("屏幕尺寸无效 ${captureW}x${captureH}")
                return
            }
            val density = resources.displayMetrics.densityDpi
            diagLog("screen=${captureW}x${captureH} rotation=$rotation density=$density")

            // 用独立帧消费线程轮询 acquireLatestImage()（见 startFrameCapture），
            // 不依赖 OnImageAvailableListener 回调，避免回调丢失导致只写出 1 帧。
            imageReader = ImageReader.newInstance(captureW, captureH, PixelFormat.RGBA_8888, 3)
            virtualDisplay = projection.createVirtualDisplay(
                "YoloTouchCapture",
                captureW, captureH, density,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                imageReader!!.surface, null, null
            )
            if (virtualDisplay == null) {
                showError("虚拟显示创建失败")
                return
            }
            // 尽早创建共享内存文件（在解压/拉起 imgui 之前）。
            // 之前 initShm 放在 prepareNativeAndRun（ioExecutor 单线程）里，
            // 解压耗时期间 ImageReader 已产帧，但 shmFile 尚未就绪导致早期帧全被丢弃；
            // 提前初始化后，录屏帧在悬浮窗拉起前即可持续写入，保证推理侧一启动就有新帧。
            try {
                initShm(File(filesDir, "frame.bin"))
            } catch (e: Exception) {
                diagLog("ERROR: initShm failed: $e")
                showError("共享内存初始化失败：${e.message}")
                return
            }
            // 共享内存就绪后立刻启动帧消费线程（独立线程轮询 ImageReader）
            startFrameCapture()
            diagLog("virtualDisplay created, capture started")
            updateNotification("录屏中，正在准备悬浮窗...")

            setupExecutor.execute {
                try {
                    if (!prepareNativeAndRun()) {
                        // prepareNativeAndRun 内部失败时已 showError 并 stopAll，这里兜底
                        mainHandler.post { stopAll() }
                    }
                } catch (e: Exception) {
                    diagLog("ERROR: setup error: $e")
                    mainHandler.post { showError("解压/启动原生失败：${e.message}") }
                }
            }

            // 帧写入看门狗：每 3s 检查共享内存序号是否在增长。
            // 若长时间不增长 → APK 侧未产帧（录屏/ImageReader 问题），写日志+持久化状态，
            // 用户无需 adb 即可判断帧源是否卡住。
            startShmWatchdog()
        } catch (e: Exception) {
            showError("录屏启动异常：${e.javaClass.simpleName} ${e.message}")
        }
    }

    // ─── 解压原生文件 + 拉起 imgui ──────────────────────────
    private fun prepareNativeAndRun(): Boolean {
        // 0. 检查 root 环境与授权（KernelSU/Magisk）
        if (!RootHelper.isAvailable()) {
            showError("未检测到 su，请确认已安装 KernelSU/Magisk")
            return false
        }
        if (!RootHelper.hasRootAccess()) {
            showError("尚未获得 root 授权，请在 KernelSU/Magisk 中允许本应用")
            return false
        }
        diagLog("root 环境正常，已获得 root 授权")
        updateNotification("录屏中，root 已就绪，正在准备悬浮窗...")

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

        // 3. 解压全部模型到 models/ 目录（面板可按需切换），并解压类别 labels 文件（.txt）
        val modelDir = File(dir, "models").apply { mkdirs() }
        val modelNames = assets.list("$ASSET_NATIVE_DIR/models") ?: emptyArray()
        for (name in modelNames) {
            if (!name.endsWith(".tflite") && !name.endsWith(".txt")) continue
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

        // 4. 共享内存文件在 startProjection 里已提前创建（initShm），
        //    帧写入也已在 startFrameCapture 的独立线程上持续进行。
        //    这里不能再调用 initShm（会 delete+重建文件）——解压期间可能已有
        //    帧写入在写该文件，重建会导致文件被删除/截断、与写帧线程竞争。
        //    只确保 imgui（root 进程）可读可写。
        val shm = File(filesDir, "frame.bin")
        shm.setReadable(true, false)
        shm.setWritable(true, false)

        // 5. 以 root 拉起 imgui（使用 RootHelper 封装）
        val ldPath = libDir.absolutePath
        diagLog("launching imgui from ${binDir.absolutePath}")
        updateNotification("录屏中，正在启动悬浮窗进程...")
        val result = RootHelper.launchBackground(
            workDir = binDir.absolutePath,
            env = mapOf("LD_LIBRARY_PATH" to ldPath),
            executable = imgui.absolutePath,
            model.absolutePath, shm.absolutePath, binDir.absolutePath
        )
        diagLog("root launch: rc=${result.exitCode} out=${result.output} err=${result.error}")

        // 帧写入由 startFrameCapture 的独立线程持续进行
        // 拉起后延迟 3s 检查 imgui 是否存活；若崩溃，把 /data/local/tmp/imgui.log 的关键
        // 错误带到通知栏，用户无需 adb 即可看到失败原因。
        mainHandler.postDelayed({ checkImguiAlive() }, 3000)
        return true
    }

    private fun checkImguiAlive() {
        if (!running.get()) return
        val alive = RootHelper.exec("pgrep -f 'bin/imgui'", timeoutSec = 3).success
        if (alive) {
            diagLog("imgui process alive")
            writeStatus("运行中：录屏正常，悬浮窗已启动")
            updateNotification("录屏中，悬浮窗运行中")
        } else {
            val log = try {
                File("/data/local/tmp/imgui.log").takeIf { it.exists() }?.readText() ?: ""
            } catch (_: Exception) { "" }
            val brief = log.replace("\n", " | ").takeLast(400)
            diagLog("imgui NOT alive! log=$brief")
            showError("悬浮窗进程启动失败：$brief")
        }
    }

    private fun initShm(file: File) {
        try {
            if (file.exists()) file.delete()
        } catch (_: Exception) {}
        val crop = activeCrop.coerceAtMost(minOf(captureW, captureH))
        activeCropOffsetX = (captureW - crop) / 2
        activeCropOffsetY = (captureH - crop) / 2
        val sizePerFrame = crop * crop * 4
        val total = SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(total.toLong())
            val hdr = ByteBuffer.allocate(SHM_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            hdr.putInt(SHM_MAGIC.toInt())
            hdr.putInt(captureW)
            hdr.putInt(captureH)
            hdr.putInt(crop * 4)         // rowStride（裁剪后）
            hdr.putInt(4)                 // pixelStride
            hdr.putInt(rotation)
            hdr.putInt(sizePerFrame)      // sizePerFrame
            hdr.putInt(0)                 // lastSeq
            hdr.putInt(crop)              // cropSize
            hdr.putInt(activeCropOffsetX) // cropOffsetX
            hdr.putInt(activeCropOffsetY) // cropOffsetY
            hdr.putInt(0)                 // cropRequest
            hdr.putLong(0)                // writeAttempts
            hdr.putLong(0)                // writeSuccesses
            hdr.putLong(0)                // acquireNulls
            hdr.putLong(0)                // writeFails
            raf.seek(0)
            raf.write(hdr.array())
        }
        shmFile = file
        // 保持一个持久 RAF 供帧线程每帧复用（避免每帧 open/close 文件）
        try { shmRaf?.close() } catch (_: Exception) {}
        shmRaf = RandomAccessFile(file, "rw")
        Log.d(TAG, "shm initialized size=$total crop=$crop")
    }

    // ─── 写帧（独立帧消费线程轮询 ImageReader） ──────────────
    // 不依赖 OnImageAvailableListener 回调 —— 该回调在部分设备/系统上可能只触发一次
    // 或丢失，导致 ImageReader 缓冲被占满后 producer 停摆，共享内存永远只有 1 帧
    // （推理 FPS=0）。用独立线程轮询 acquireLatestImage() 总能持续消费新帧。
    private fun startFrameCapture() {
        frameThread = Thread {
            while (running.get()) {
                try {
                    val reader = imageReader ?: break
                    val image = reader.acquireLatestImage()
                    if (image == null) {
                        acquireNulls.incrementAndGet()
                        // 无新帧时也定期把诊断计数刷到头部（不动 lastSeq），
                        // 让 C++ 面板能看到实时的 acquireNulls/writeAttempts 增长，
                        // 从而区分“帧线程存活但虚拟屏未产帧(屏幕静止)”与“帧线程已死”。
                        val now = System.currentTimeMillis()
                        if (now - lastDiagFlushMs >= 500) {
                            lastDiagFlushMs = now
                            flushDiagnostics()
                        }
                        Thread.sleep(10) // 避免 CPU 空转
                        continue
                    }
                    writeAttempts.incrementAndGet()
                    writeFrame(image)   // 内部会 close
                } catch (e: Exception) {
                    writeFails.incrementAndGet()
                    diagLog("frame capture error: ${e.message}")
                    Thread.sleep(100)
                }
            }
        }.apply { start() }
    }

    // 把当前诊断计数写入共享内存头部（保持 lastSeq 不变，C++ 不会误判为新帧）。
    // 在无新帧时由帧线程每 500ms 调用一次，让面板能看到实时的写帧/取帧计数变化。
    // 注意：必须保留 C++ 写入的 cropRequest（offset 44）——若在无新帧期间把它
    // 清成 0，用户切换裁剪尺寸的请求会在这段时间内被丢失、永远不会被应用。
    private fun flushDiagnostics() {
        val raf = shmRaf ?: return
        try {
            val w = if (lastW > 0) lastW else captureW
            val h = if (lastH > 0) lastH else captureH
            val crop = activeCrop.coerceAtMost(minOf(w, h))
            val rs = if (lastRowStride > 0) lastRowStride else crop * 4
            val spf = if (lastSizePerFrame > 0) lastSizePerFrame else crop * crop * 4
            val offX = (w - crop) / 2
            val offY = (h - crop) / 2
            // 保留 C++ 侧尚未消费的裁剪请求
            var req = 0
            raf.seek(44L)
            if (raf.length() >= 48L) req = readIntLE(raf)
            raf.seek(0)
            val hdr = ByteBuffer.allocate(SHM_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            hdr.putInt(SHM_MAGIC.toInt())
            hdr.putInt(w)
            hdr.putInt(h)
            hdr.putInt(rs)
            hdr.putInt(4)            // pixelStride
            hdr.putInt(rotation)
            hdr.putInt(spf)
            hdr.putInt(lastSeq.get().toInt())
            hdr.putInt(crop)
            hdr.putInt(offX)
            hdr.putInt(offY)
            hdr.putInt(req)          // 保留 C++ 的裁剪请求
            hdr.putLong(writeAttempts.get())
            hdr.putLong(writeSuccesses.get())
            hdr.putLong(acquireNulls.get())
            hdr.putLong(writeFails.get())
            raf.write(hdr.array())
        } catch (_: Exception) {}
    }

    // 帧写入看门狗：周期性检查共享内存序号是否在增长。
    // - 增长正常：保持运行状态
    // - 长时间不增长：帧源卡住（录屏/ImageReader/写帧线程问题），写日志便于诊断
    private var shmWatchdogSeq = 0L
    private var shmStallCount = 0

    private fun startShmWatchdog() {
        mainHandler.post(object : Runnable {
            override fun run() {
                if (!running.get()) return
                val s = lastSeq.get()
                if (s == shmWatchdogSeq) {
                    shmStallCount++
                    if (shmStallCount >= 4) {  // ~12s 无新帧
                        diagLog("WARN: 帧源序号停滞于 $s（${shmStallCount * 3}s 无新帧）" +
                            "——录屏/ImageReader 可能未产帧")
                    }
                } else {
                    shmStallCount = 0
                }
                shmWatchdogSeq = s
                mainHandler.postDelayed(this, 3000)
            }
        })
    }

    private fun writeFrame(image: Image) {
        // 句柄/文件未就绪时直接关闭退出（在 try 之外，避免 finally 二次 close）
        val shm = shmFile ?: run { image.close(); return }
        if (shmRaf == null) {
            try {
                shmRaf = RandomAccessFile(shm, "rw")
            } catch (e: Exception) {
                writeFails.incrementAndGet()
                image.close()
                return
            }
        }
        val raf = shmRaf ?: run { image.close(); return }
        try {
            val plane = image.planes[0]
            val buf = plane.buffer
            val imgW = image.width
            val imgH = image.height
            val srcRowStride = plane.rowStride
            val ps = plane.pixelStride

            // 读取 C++ 面板请求的裁剪尺寸（cropRequest，offset 44）。每帧都读，
            // 读到合法新值立即应用（不做 delete+重建，C++ 侧持有旧文件 fd 会失效）。
            var crop = activeCrop
            if (raf.length() >= 48L) {
                raf.seek(44L)
                val req = readIntLE(raf)
                if (req != 0 && req != activeCrop && CROP_OPTIONS.contains(req)) {
                    diagLog("C++ 请求切换裁剪尺寸: $req (原 ${activeCrop})")
                    activeCrop = req
                    crop = req
                }
            }
            crop = crop.coerceAtMost(minOf(imgW, imgH))
            val offX = (imgW - crop) / 2
            val offY = (imgH - crop) / 2
            val cropStride = crop * ps
            val sizePerFrame = crop * crop * ps

            // 裁剪尺寸变大时扩展文件（C++ 侧也会自动扩展）
            val need = SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT
            if (raf.length() < need.toLong()) raf.setLength(need.toLong())
            // 裁剪缓冲不够大时扩容
            if (cropBuffer.size < sizePerFrame) {
                cropBuffer = ByteArray(sizePerFrame)
            }

            val seq = lastSeq.incrementAndGet()
            val bufIdx = ((seq % SHM_BUFFER_COUNT).toInt()) * sizePerFrame

            // 保存最近一帧信息供 flushDiagnostics 用
            lastW = imgW
            lastH = imgH
            lastRowStride = cropStride
            lastSizePerFrame = sizePerFrame

            // 一次内存拷贝：把裁剪区域逐行拷贝到 cropBuffer
            // 而非逐行 seek+write（系统调用开销极大）
            val pos = buf.position()
            buf.rewind()
            val dst = cropBuffer
            for (r in 0 until crop) {
                buf.position((offY + r) * srcRowStride + offX * ps)
                buf.get(dst, r * cropStride, cropStride)
            }
            buf.position(pos)

            // 一次 write() 写入整个裁剪帧
            raf.seek((SHM_HEADER_SIZE + bufIdx).toLong())
            raf.write(dst, 0, sizePerFrame)

            // 更新 header（全屏尺寸 + 裁剪信息 + 序号 + 写帧诊断计数）
            writeSuccesses.incrementAndGet()
            raf.seek(0)
            val hdr = ByteBuffer.allocate(SHM_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            hdr.putInt(SHM_MAGIC.toInt())
            hdr.putInt(imgW)
            hdr.putInt(imgH)
            hdr.putInt(cropStride)
            hdr.putInt(ps)
            hdr.putInt(rotation)
            hdr.putInt(sizePerFrame)
            hdr.putInt(seq.toInt())
            hdr.putInt(crop)
            hdr.putInt(offX)
            hdr.putInt(offY)
            hdr.putInt(0) // cropRequest 每帧已消费，回写 0
            hdr.putLong(writeAttempts.get())
            hdr.putLong(writeSuccesses.get())
            hdr.putLong(acquireNulls.get())
            hdr.putLong(writeFails.get())
            raf.write(hdr.array())

            if (lastSeq.get() % 60 == 0L) Log.d(TAG, "captured seq=${lastSeq.get()}")
        } catch (e: Exception) {
            writeFails.incrementAndGet()
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
        try { shmRaf?.close() } catch (_: Exception) {}
        shmRaf = null
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
        setupExecutor.shutdownNow()
        try { frameThread?.join(1000) } catch (_: Exception) {}
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
