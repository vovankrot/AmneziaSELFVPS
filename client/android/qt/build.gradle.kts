plugins {
    id(libs.plugins.android.library.get().pluginId)
    id("property-delegate")
}

val amneziaJavaToolchainVersion = providers.gradleProperty("amneziaJavaToolchainVersion")
    .orElse(providers.environmentVariable("AMNEZIA_JAVA_TOOLCHAIN_VERSION"))
    .orElse("17")
    .get()
    .toInt()

java {
    toolchain.languageVersion.set(JavaLanguageVersion.of(amneziaJavaToolchainVersion))
}

val qtAndroidDir: String by gradleProperties

android {
    namespace = "org.qtproject.qt.android.binding"

    sourceSets {
        getByName("main") {
            java.setSrcDirs(listOf("$qtAndroidDir/src"))
            res.setSrcDirs(listOf("$qtAndroidDir/res"))
        }
    }
}

dependencies {
    api(fileTree(mapOf("dir" to "../libs", "include" to listOf("*.jar"))))
}
