# ImGui Overlay (纯 C/C++ 悬浮窗 · 支持 Android 16 · GitHub 云端编译)

将原「imgui 安卓纯 C 开发环境模板」改造为 **Android APK**，可在 **Android 16（API 36）** 上以 root 运行，
并保留原工程全部特性：

- ✅ **同时穿透触摸悬浮窗和屏幕**：原生层 `EVIOCGRAB` 独占抓取触摸屏 → 喂给 ImGui，同时通过 `/dev/uinput`
  虚拟设备把触摸**原样转发**到下层 App，实现「悬浮窗拦截 + 其余穿透」。
- ✅ **libgui 私有 API 直接创建全屏 Surface**（`SurfaceComposerClient` + trusted overlay），无需 Activity/WindowManager。
- ✅ **Vulkan 渲染**（`vulkan_wrapper` 动态加载 + SwapChain），也可切换 OpenGL。
- ✅ **中文支持**：内嵌 OPPOSans 字体 + `GetGlyphRangesChineseFull()`。
- ✅ **防截图**：trusted overlay + skipScreenshot。
- ✅ **GitHub Actions 云端编译**：push 即自动出 APK。

---

## 一、Android 15/16 兼容性修复

Android 15/16 起，`LayerMetadata` 被移入 `android::gui` 命名空间，libgui 的 C++ 符号发生改变：

| 符号 | Android ≤ 14 | Android 15/16 |
|------|--------------|---------------|
| `LayerMetadata` 构造 | `_ZN7android13LayerMetadataC2Ev` | `_ZN7android3gui13LayerMetadataC2Ev` |
| `createSurface` | `...ENS_13LayerMetadataEPj` | `...ENS_3gui13LayerMetadataEPj` |

`app/src/main/cpp/include/native_surface/ANativeWindowCreator.h` 已：

1. 为 Android 15/16 添加对应符号（与 14 相同的 `gui::LayerMetadata`）。
2. 将补丁查找改为「取 ≤ 当前系统版本的最大条目」，对未来版本自动回退。

> 若真机上仍报 `Failed to resolve symbol`，用 `adb logcat` 查看具体缺失符号，参考
> [android-native-window-symbol-patcher](https://github.com/mohamad-aljeiawi/android-native-window-symbol-patcher)
> 用 `llvm-nm`/`llvm-cxxfilt` 提取新符号名补充即可。

---

## 二、环境要求

- **已 root 设备**（Magisk/KernelSU 等），Android 9 ~ Android 16
- **arm64-v8a**（Android 16 强制 64 位）
- 原生层需要 `/dev/input`、`/dev/uinput` 访问权限（root 提供）

---

## 三、GitHub Actions 云端编译（推荐）

1. 将本仓库推送到 GitHub。
2. Actions → `Build APK (Android 16)`（push 自动触发，也可 `workflow_dispatch` 手动）。
3. 完成后在 Artifacts 下载：
   - `imgui-overlay-apk`：`app-debug.apk` / `app-release.apk`
   - `imgui_overlay-executable`：原生可执行文件（便于 `adb` 直接推送调试）

---

## 四、本地编译

环境：JDK 17、Android SDK（platform 36 + build-tools 36 + NDK 27.1.12297006）、cmake、ninja。

```bash
export ANDROID_HOME=/path/to/android-sdk
./gradlew assembleDebug
# 产物：app/build/outputs/apk/debug/app-debug.apk
```

> 原生可执行文件由自定义 Gradle 任务 `buildNativeBinary` 通过 NDK 交叉工具链直接构建
> （`add_executable`），产物打包进 APK 的 `assets/bin/imgui_overlay`。

---

## 五、安装与使用

1. 安装 `app-release.apk`，打开 App。
2. 授予 root 权限（Magisk 弹窗点允许）。
3. 点「启动悬浮窗」：App 把原生二进制部署到 `/data/local/tmp` 并后台运行。
4. 停止：点「停止悬浮窗」。

日志：`/data/local/tmp/imgui.log`（进程日志）、`/data/local/tmp/imgui.pid`（进程号）。

也可跳过 App，直接用 adb 运行原生二进制：

```bash
adb push app/build/nativeBuild/arm64-v8a/imgui_overlay /data/local/tmp/
adb shell "chmod 755 /data/local/tmp/imgui_overlay && /data/local/tmp/imgui_overlay"
```

---

## 六、切换渲染后端

默认 Vulkan。如需 OpenGL，编辑 `app/src/main/cpp/src/main.cpp`，在 CMake 中
对目标增加 `-DUSE_OPENGL`，并把 `cmake/CMakeLists.txt` 的源文件切换为
`imgui_impl_opengl3.cpp` + `imgui_image.cpp`（参照原 `Android.mk`）。

---

## 七、YOLO 推理 + 折叠控制面板

支持 **TFLite int8 量化模型 + 骁龙（Hexagon）NPU 加速**（NNAPI delegate，框架 A）：

- 数据来源：root 实时采集屏幕（`screencap`）。
- 检测框按**真实屏幕坐标**直接描边叠加在透明全屏悬浮层上，不遮挡真实屏幕。
- 控制面板为**单个可收起面板**：点击「收起 <<」缩成图标，再点图标展开。
  面板内含四组折叠分区：
  - **推理参数**：检测开关、置信度、IOU。
  - **模型与显示**：模型路径、显示检测框、显示标签。
  - **类别设置**：类别名称（逗号分隔，按类别ID顺序）、启用类别过滤、仅显示的类别ID。
  - **性能信息**：帧率、推理耗时、总耗时、检测数量。
- 右上角「退出」按钮可结束悬浮窗（结束 imgui 进程）。

### 用前准备

1. 编译时需在 GitHub 云端（workflow 已内置 TFLite 编译步骤），本地构建默认关闭 YOLO。
2. 把模型放到设备目录（App 启动时自动创建）：

```bash
adb push valorant_256_v26n.tflite /data/local/tmp/models/
```

3. 打开面板 → 输入模型路径（默认 `/data/local/tmp/models/valorant_256_v26n.tflite`）→ 勾选「启用检测」→ 点「加载模型」。
4. 在「类别设置」里按类别 ID 顺序填写类别名称（如 `player,enemy,head`），需要筛选时勾选「启用类别过滤」并填入要显示的类别 ID（如 `0,1`）。

> 说明：模型需为 **uint8 int8 量化**；NNAPI 命中 NPU 为尽力而为，个别算子回退 CPU 不影响运行。
> 若需每一层都强制走 NPU，需改用 QNN 方案（本版本不启用）。

---

## 八、目录结构

```
app/src/main/cpp/                       # 原生 C/C++ 层（原 jni 移植）
  include/native_surface/ANativeWindowCreator.h   # libgui 私有 API + Android15/16 适配
  include/Android_touch/TouchHelperA.h            # EVIOCGRAB + uinput 触摸穿透
  src/Android_touch/TouchHelperA.cpp
  src/Android_vulkan/vulkan_wrapper.cpp           # 动态加载 Vulkan
  src/Android_vulkan/VulkanUtils.cpp              # Vulkan 初始化/渲染
  src/Yolo/YoloDetector.cpp                       # TFLite int8 + NNAPI 推理
  src/Yolo/ScreenCapture.cpp                      # root 屏幕采集
  src/ImGui/…                                      # ImGui 内核 + 后端
  include/ImGui/font/Font.h                        # OPPOSans 中文字体
app/src/main/java/…/MainActivity.kt               # root 启动器
.github/workflows/build-apk.yml                   # GitHub 云端编译
```

## 九、风险与说明

- 原生可执行文件依赖 libgui **私有符号**，非稳定 NDK API，Android 版本升级可能需补符号。
- 触摸 Grab 会独占触摸屏，由 uinput 转发出，若下层 App 触摸异常请先「停止悬浮窗」。
- 仅供学习/个人使用，请遵守当地法律法规。