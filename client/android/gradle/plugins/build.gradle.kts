plugins {
    `kotlin-dsl`
}

repositories {
    gradlePluginPortal()
}

val amneziaJavaToolchainVersion = providers.gradleProperty("amneziaJavaToolchainVersion")
    .orElse(providers.environmentVariable("AMNEZIA_JAVA_TOOLCHAIN_VERSION"))
    .orElse("17")
    .get()
    .toInt()

kotlin {
    jvmToolchain(amneziaJavaToolchainVersion)
}

gradlePlugin {
    plugins {
        register("settingsGradlePropertyDelegate") {
            id = "settings-property-delegate"
            implementationClass = "SettingsPropertyDelegate"
        }

        register("projectGradlePropertyDelegate") {
            id = "property-delegate"
            implementationClass = "ProjectPropertyDelegate"
        }
    }
}
