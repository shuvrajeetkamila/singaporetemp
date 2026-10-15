plugins {
    id("com.android.application")
}

// Release signing is optional. CI sets these environment variables only when the
// repository secrets exist. Without them, the release bundle is built unsigned.
val releaseKeystore: String? = System.getenv("RELEASE_KEYSTORE_PATH")

android {
    namespace = "sg.onetoone.island"
    compileSdk = 35

    defaultConfig {
        applicationId = "sg.onetoone.island"
        minSdk = 26
        targetSdk = 35
        versionCode = (System.getenv("GITHUB_RUN_NUMBER") ?: "1").toInt()
        versionName = "0.1.0-prototype"
    }

    assetPacks += listOf(":island")

    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = file(releaseKeystore)
                storePassword = System.getenv("RELEASE_STORE_PASSWORD")
                keyAlias = System.getenv("RELEASE_KEY_ALIAS")
                keyPassword = System.getenv("RELEASE_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (releaseKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.webkit:webkit:1.11.0")
}
