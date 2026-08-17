plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "hualai.yolo"
    compileSdk = 34

    defaultConfig {
        applicationId = "hualai.yolo"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    // release 签名：优先使用 CI/环境注入的 keystore，否则回退 debug 签名，
    // 确保产出的 APK 始终可安装（避免 unsigned APK 无法安装/启动闪退）
    signingConfigs {
        create("release") {
            val ks = System.getenv("KEYSTORE_PATH")
            if (!ks.isNullOrBlank() && File(ks).exists()) {
                storeFile = file(ks)
                storePassword = System.getenv("KEYSTORE_PASSWORD") ?: "yolotouch"
                keyAlias = System.getenv("KEY_ALIAS") ?: "yolotouch"
                keyPassword = System.getenv("KEY_PASSWORD") ?: "yolotouch"
            } else {
                storeFile = file(
                    (System.getenv("ANDROID_HOME") ?: System.getProperty("user.home")) +
                        "/.android/debug.keystore"
                )
                storePassword = "android"
                keyAlias = "androiddebugkey"
                keyPassword = "android"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    // 原生二进制（imgui + .so + 模型）由 CI 在构建前拷入 assets/native，不纳入版本管理
    sourceSets["main"].assets.srcDir("src/main/assets")
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
}
