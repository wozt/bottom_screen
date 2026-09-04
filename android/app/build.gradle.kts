// AGP 9 compiles Kotlin itself; applying org.jetbrains.kotlin.android as
// well makes both try to register a "kotlin" extension and the build
// stops before it starts.
plugins {
    id("com.android.application")
}

android {
    namespace = "fr.wozt.bottomscreen"
    compileSdk = 36

    defaultConfig {
        applicationId = "fr.wozt.bottomscreen"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
    kotlin {
        jvmToolchain(21)
    }
    sourceSets["main"].kotlin.srcDir("src/main/kotlin")
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")

    /* The wire format is a pure function of its bytes, so it is testable
     * without a device. A test that needs a phone plugged in is a test
     * nobody runs. */
    testImplementation("junit:junit:4.13.2")
}
