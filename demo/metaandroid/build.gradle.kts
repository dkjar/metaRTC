plugins {
    // AGP 8.11.x 需要 Gradle 8.13+ / JDK 17+；与 Kotlin 2.2.x 的 KMP 官方兼容区间一致
    id("com.android.application") version "8.11.1" apply false
    id("com.android.library") version "8.11.1" apply false
    id("org.jetbrains.kotlin.multiplatform") version "2.2.20" apply false
    id("org.jetbrains.kotlin.android") version "2.2.20" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.20" apply false
}
