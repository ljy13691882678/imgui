plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// 原生可执行文件的输出目录（打包进 APK 的 assets/bin）
val nativeAssetsDir = layout.buildDirectory.dir("native-assets")
// 原生 CMake 构建目录
val nativeBuildDir = layout.buildDirectory.dir("nativeBuild")

android {
    namespace = "com.example.imgui_overlay"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.example.imgui_overlay"
        minSdk = 21
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        // Android 16 要求 64 位设备，仅构建 arm64-v8a
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // CI 无签名证书，复用 debug 签名以便能直接安装
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    ndkVersion = "27.1.12297006"

    // 注册原生可执行文件的输出目录为 assets 源目录
    sourceSets {
        getByName("main") {
            assets.srcDir(nativeAssetsDir)
        }
    }
}

// ---------------------------------------------------------------------------
// 纯 C/C++ 可执行文件：用 NDK 的 CMake 交叉工具链直接构建（add_executable），
// 产物收集到 assets/bin 并打包进 APK。
// ---------------------------------------------------------------------------
val abi = "arm64-v8a"

val buildNativeBinary by tasks.registering {
    val ndkDir = android.ndkDirectory
    val toolchainFile = File(ndkDir, "build/cmake/android.toolchain.cmake")
    val srcDir = project.layout.projectDirectory.dir("src/main/cpp")
    val outDir = nativeBuildDir.get().dir(abi)

    inputs.dir(srcDir)
    outputs.file(outDir.dir("imgui_overlay"))

    doLast {
        outDir.asFile.mkdirs()
        // 1) 配置
        exec {
            workingDir = outDir.asFile
            commandLine(
                "cmake",
                "-S", srcDir.asFile.absolutePath,
                "-B", outDir.asFile.absolutePath,
                "-G", "Ninja",
                "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DANDROID_ABI=$abi",
                "-DANDROID_PLATFORM=android-21",
                "-DANDROID_STL=c++_static",
                "-DANDROID_ARM_NEON=ON"
            )
        }
        // 2) 编译
        exec {
            workingDir = outDir.asFile
            commandLine("cmake", "--build", outDir.asFile.absolutePath, "--target", "imgui_overlay")
        }
    }
}

val collectNativeBinary by tasks.registering(Copy::class) {
    dependsOn(buildNativeBinary)
    from(nativeBuildDir)
    include("**/imgui_overlay")
    into(nativeAssetsDir.get().dir("bin"))
    rename { "imgui_overlay" }
    duplicatesStrategy = DuplicatesStrategy.INCLUDE
}

// 确保 assets 合并发生在原生二进制收集之后
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("Assets")) {
        dependsOn(collectNativeBinary)
    }
}