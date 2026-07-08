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
    namespace = "org.amnezia.vpn.protocol.awg"
}

dependencies {
    compileOnly(project(":utils"))
    compileOnly(project(":protocolApi"))
    implementation(project(":wireguard"))
}
