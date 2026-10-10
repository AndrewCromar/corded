plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "org.corded.corded_app"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "org.corded.app"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        // Android 9: the first version with everything the core needs.
        minSdk = 28
        targetSdk = flutter.targetSdkVersion
        // Uses the version code from pubspec.yaml. When using split APKs, 1000 * ABI_VERSION
        // is added automatically by Flutter. (https://developer.android.com/studio/build/configure-apk-splits#configure-APK-versions)
        // You can force using the value of versionCode by specifying the `-P force-version-code-ignoring-abi=true`
        // flag during build.
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    // Android only installs an update over an app signed with the same key.
    // CORDED_KEYSTORE names a keystore file and CORDED_KEYSTORE_PASSWORD opens
    // it; without them the build is signed with this machine's debug key and
    // can only replace builds made on the same machine.
    val keystore = System.getenv("CORDED_KEYSTORE")
    signingConfigs {
        if (!keystore.isNullOrEmpty()) {
            create("corded") {
                storeFile = file(keystore)
                storePassword = System.getenv("CORDED_KEYSTORE_PASSWORD")
                keyAlias = "corded"
                keyPassword = System.getenv("CORDED_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName(if (keystore.isNullOrEmpty()) "debug" else "corded")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
