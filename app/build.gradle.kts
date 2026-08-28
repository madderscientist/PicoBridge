plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.madderscientist.picobridge"
    compileSdk {
        version = release(37)
    }

    // 必须与已安装的 NDK 一致，否则 AGP 会去下载另一个版本
    ndkVersion = "30.0.16138531"

    defaultConfig {
        applicationId = "com.madderscientist.picobridge"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}