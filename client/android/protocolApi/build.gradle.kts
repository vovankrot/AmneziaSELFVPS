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
    namespace = "org.amnezia.vpn.protocol"
}

dependencies {
    compileOnly(project(":utils"))
    implementation(libs.androidx.annotation)
    implementation(libs.kotlinx.coroutines)
}
