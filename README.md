# imgui 安卓纯 C++ 开发环境模板

基于 Android NDK + Dear ImGui 的安卓原生 GUI 模板（纯 C++，可执行文件形式，非 .so）。
支持 **Vulkan** 与 **OpenGL ES 3** 两种渲染后端，通过 `jni/Android.mk` 中的 `OPENGL_DRAW` 切换。

## 环境要求

- 源码目录：`_extracted/安卓环境/jni/`
- 构建系统：Android NDK `ndk-build`（项目使用 Android.mk / Application.mk 老式构建）
- ABI：`arm64-v8a`，最低 API 21
- 运行设备：已 Root 的 Android 设备（使用 SurfaceFlinger 私有接口直接创建全屏图层）

## 在 GitHub Actions 上编译

仓库已配置 `.github/workflows/build.yml`：

1. 将代码推送到 `main` 分支（或手动触发 `workflow_dispatch`）；
2. 在仓库 **Actions** 页面选择 `Build Android ImGui Executable`；
3. 构建完成后进入对应运行，在 **Artifacts** 处下载：
   - `imgui-vulkan-arm64`（默认，推荐）
   - `imgui-opengl-arm64`

产物为可执行文件：
- Vulkan：`_extracted/安卓环境/libs/arm64-v8a/imgui_chain_1_3_vk`
- OpenGL：`_extracted/安卓环境/libs/arm64-v8a/imgui_chain_1_3_op`

## 本地编译（需要已安装 NDK）

```bash
export NDK=/path/to/android-ndk-r25c
export PATH=$NDK:$PATH
cd "_extracted/安卓环境"
ndk-build -j2
```

默认编译 Vulkan 版；如需 OpenGL 版，将 `jni/Android.mk` 中 `OPENGL_DRAW` 改为 `1`。

## 在 Root 的 Android 16 设备上运行

1. 手机开启 **USB 调试**，电脑连接后：

   ```bash
   adb push _extracted/安卓环境/libs/arm64-v8a/imgui_chain_1_3_vk /data/local/tmp/
   adb shell
   su          # 进入 root（Magisk / KernelSU）
   chmod 755 /data/local/tmp/imgui_chain_1_3_vk
   /data/local/tmp/imgui_chain_1_3_vk
   ```

2. 运行期间请保持屏幕点亮（程序通过 SurfaceFlinger 在屏幕上创建全屏图层）。
3. 结束运行：`Ctrl+C` 或另开终端 `adb shell su -c "killall imgui_chain_1_3_vk"`。

> 注意：该模板通过 `libgui` 私有 API（`ANativeWindowCreator`）在 shell/root 环境直接创建窗口，
> 属于 root 级系统调用技术，依赖系统私有符号。不同 Android 版本（尤其大版本升级）可能因 API 变更需要适配。
