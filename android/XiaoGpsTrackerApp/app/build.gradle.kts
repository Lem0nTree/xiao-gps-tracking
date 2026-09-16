plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.example.xiaogpstracker"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.example.xiaogpstracker"
        minSdk = 26
        targetSdk = 36
        versionCode = 22
        versionName = "2.1.1"
    }

    buildFeatures {
        viewBinding = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("org.maplibre.gl:android-sdk:11.8.0")
    testImplementation("junit:junit:4.13.2")
}
