plugins {
    id("com.android.application")
}

android {
    namespace = "com.saintsm.smoketest"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.saintsm.smoketest"
        minSdk = 27
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0-smoke"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-fno-omit-frame-pointer")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }
}
