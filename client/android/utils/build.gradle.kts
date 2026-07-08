plugins {
    id(libs.plugins.android.library.get().pluginId)
    id(libs.plugins.kotlin.android.get().pluginId)
}

val amneziaJavaToolchainVersion = providers.gradleProperty("amneziaJavaToolchainVersion")
    .orElse(providers.environmentVariable("AMNEZIA_JAVA_TOOLCHAIN_VERSION"))
    .orElse("17")
    .get()
    .toInt()

kotlin {
    jvmToolchain(amneziaJavaToolchainVersion)
}

android {
    namespace = "org.amnezia.vpn.util"

    buildFeatures {
        // add BuildConfig class
        buildConfig = true
    }
}

dependencies {
    implementation(libs.androidx.core)
    implementation(libs.kotlinx.coroutines)
    implementation(libs.androidx.security.crypto)
}
