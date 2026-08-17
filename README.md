# YoloTouch ImGui — 安卓自瞄悬浮窗

基于 Android NDK + Dear ImGui + TFLite/QNN 的安卓原生自瞄辅助应用。

- **APK 层**（Kotlin/Java）：MediaProjection 录屏 → 共享内存传帧 + 卡密验证
- **Native 层**（C++）：ImGui 悬浮窗 + 目标检测推理 + uinput/内核触摸注入 + 卡密验证
- 支持 **Vulkan** 与 **OpenGL ES 3** 两种渲染后端，通过 `imgui_overlay/Android.mk` 中的 `OPENGL_DRAW` 切换
- **T3 网络验证防破解**：APK 与二进制使用同一套配置（调用码 + APPKEY + RSA 公钥）独立验证同一张卡密，任一侧被绕过都无法运行

## 项目结构

```
YoloTouchImgui/
├── app/                          # Android APK 模块（Kotlin）
│   └── src/main/
│       ├── java/com/yolotouch/imgui/
│       │   ├── MainActivity.kt   # 主界面：卡密验证 + 录屏授权 + 启动/停止
│       │   ├── CaptureService.kt # 前台服务：录屏 → 共享内存 → 解压并拉起 native（传卡密）
│       │   ├── T3AuthManager.kt  # 卡密登录 / 持久化 / 心跳保活封装
│       │   ├── T3Config.kt       # T3 后台配置（与 native 一致）
│       │   ├── T3Verify.java     # T3 Java SDK（对接示例：Android 全屏验证）
│       │   └── RootHelper.kt     # su 命令封装（KernelSU/Magisk）
│       ├── AndroidManifest.xml
│       └── res/                  # 布局/资源
├── imgui_overlay/                # Native C++ 可执行文件（ndk-build）
│   ├── Android.mk                # 构建脚本（OPENGL_DRAW=0/1 切换后端）
│   ├── Application.mk
│   ├── src/
│   │   ├── main.cpp              # 主循环：入口卡密验证 + ImGui 面板 + 推理 + 自瞄 + 注入
│   │   ├── auth/                 # T3 卡密验证封装（t3auth.cpp/h，复用同一套配置）
│   │   ├── t3sdk/                # T3 C++ SDK（对接示例：Android JNI 命令行）
│   │   ├── inference/            # 推理引擎（TFLite LiteRT / QNN）
│   │   ├── injection/            # 触摸注入（uinput + TimeDriver 内核）
│   │   ├── Android_draw/         # SurfaceFlinger 全屏绘图
│   │   ├── Android_touch/        # 触摸事件读取
│   │   └── ImGui/                # Dear ImGui 核心 + 后端
│   ├── include/                  # 头文件（ImGui / TFLite / QNN / flatbuffers）
│   ├── lib/arm64-v8a/            # 预编译 .so（TFLite + QNN HTP/DSP/GPU）
│   └── driver/arm64-v8a/         # 预编译 TimeDriver 内核驱动静态库
├── models/                       # TFLite 模型文件（.tflite + .txt 标签）
├── build.gradle.kts              # Gradle 根配置
├── gradlew / gradlew.bat         # Gradle wrapper
└── release.jks                   # 签名文件（CI 使用）
```

## 环境要求

- 构建系统：Android NDK r25c（ndk-build）+ Gradle（JDK 17）
- ABI：`arm64-v8a`，最低 API 29（Android 10）
- 运行设备：已 Root 的 Android 设备（需要 su 以 root 拉起 native 进程）

## 在 GitHub Actions 上编译

仓库已配置 `.github/workflows/build.yml`：

1. 将代码推送到 `main` 分支（或手动触发 `workflow_dispatch`）；
2. 在仓库 **Actions** 页面选择 `Build Android APK + ImGui Executable`；
3. 构建完成后在对应运行页的 **Artifacts** 处下载：
   - `YoloTouchImgui-aim-trigger-vulkan.apk`（默认，推荐）
   - `YoloTouchImgui-aim-trigger-opengl.apk`

CI 流程：
1. 编译 native 可执行文件（`ndk-build`）
2. 将可执行文件 + 模型 + .so 拷入 `app/src/main/assets/native/`
3. 用 Gradle 构建签名的 Release APK

## 本地编译

```bash
# 1. 设置 NDK 环境
export NDK=/path/to/android-ndk-r25c
export PATH=$NDK:$PATH

# 2. 编译 native 可执行文件
cd YoloTouchImgui/imgui_overlay
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
  APP_STL=c++_static APP_ABI=arm64-v8a APP_PLATFORM=android-21 -j2

# 3. 准备 APK assets
cd ..
ASSETS="app/src/main/assets/native"
mkdir -p "$ASSETS/lib" "$ASSETS/models"
cp imgui_overlay/libs/arm64-v8a/imgui "$ASSETS/imgui"
cp models/*.tflite "$ASSETS/models/"
cp models/*.txt "$ASSETS/models/"
cp imgui_overlay/lib/arm64-v8a/*.so "$ASSETS/lib/"

# 4. 编译 APK
./gradlew :app:assembleRelease
```

默认编译 Vulkan 版；如需 OpenGL 版，将 `imgui_overlay/Android.mk` 中 `OPENGL_DRAW` 改为 `1`。

## 在 Root 设备上运行

1. 安装编译好的 APK（`app/build/outputs/apk/release/app-release.apk`）
2. 打开应用，**输入卡密并点击“卡密验证”**，验证通过后才能开始使用
3. 授予 **MediaProjection 录屏权限** 和 **通知权限**
4. 确保 KernelSU / Magisk 已授予本应用 root 权限
5. 点击 **"开始"** 按钮

应用会：
1. 启动前台服务，开始 MediaProjection 录屏
2. 启动 APK 侧 T3 心跳保活（每 60s 一次，连续失败 5 次强杀 native）
3. 将帧数据写入共享内存文件
4. 从 assets 解压 native 可执行文件 + 模型 + .so
5. 通过 `su` 以 root 拉起 `imgui` 进程，并把卡密作为参数传入
6. native 在启动时用同一套 T3 配置**独立验证卡密**，验证失败立即退出
7. 验证通过后在屏幕上显示 ImGui 悬浮窗（自瞄配置面板 + 目标检测可视化）

停止：点击 **"停止"** 按钮，或从通知栏移除服务。

## 卡密验证（防破解）

采用 **T3 网络验证**（官网 [t3yanzheng.com](https://www.t3yanzheng.com)）做双重验证：

- **APK 侧**：Java SDK（`T3Verify.java`）负责卡密登录，成功后将卡密 + `statecode` 保存到本地；启动时由 `CaptureService` 传递卡密给 native 并启动心跳
- **Native 侧**：C++ SDK（`t3sdk.cpp`）在 `main()` 入口独立验证同一张卡密，验证失败进程直接退出，悬浮窗起不来；同时启动独立心跳线程
- **心跳保活**：APK 与 native 各自每 60s 心跳一次，连续失败 5 次强制退出——即使 APK 被破解跳过验证，native 也会在 5 分钟内自动下线

配置均在以下两个文件（**必须保持一致**）：

| 配置项 | native | APK |
| --- | --- | --- |
| 单码登录调用码 | `imgui_overlay/src/auth/t3auth.cpp` | `app/.../T3Config.kt` |
| 心跳调用码 | 同上 | 同上 |
| APPKEY / RSA 公钥 | 同上 | 同上 |

替换为你自己在 [T3 后台](https://www.t3yanzheng.com) 创建的调用码 / 密钥 / 公钥后即可使用。后台需开启：RSA 算法 + HEX 编码 + 时间戳校验 + 双向签名 + JSON 返回（示例注释中有完整清单）。

## 自瞄功能

- 多种目标检测模型（YOLOv8n / Valorant / CF 等）
- 多个推理后端：TFLite LiteRT、QNN HTP（Hexagon DSP/NPU 加速）
- 多种自瞄模式：准星/身体/最近目标
- 压枪辅助（陀螺仪 / uinput）
- 双摄/四摄/六摄陀螺仪平滑注入
- 可配置触发区域、灵敏度、平滑参数
- 内核触摸模式（TimeDriver 驱动，延迟更低）

## 技术要点

- **SurfaceFlinger 直接渲染**：通过 `libgui` 私有 API 在 shell/root 环境直接创建全屏图层，无需 Activity
- **共享内存通信**：APK 侧通过 `MediaProjection` 录屏写入共享内存文件，native 侧读取并推理，支持运行时动态切换裁剪尺寸
- **双缓冲帧传输**：共享内存中维护 2 帧缓冲区，APK 写帧与 native 读帧互不阻塞
- **推理引擎可切换**：支持 TFLite LiteRT（CPU/GPU/NNAPI）和 Qualcomm QNN HTP（Hexagon NPU）
- **内核陀螺仪注入**：通过 TimeDriver 内核模块实现硬件级触摸注入，延迟远低于 uinput

> 注意：该模板依赖系统私有符号（`libgui`、`SurfaceFlinger` 等），不同 Android 版本（尤其大版本升级）可能因 API 变更需要适配。