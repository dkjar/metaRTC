plugins {
    // 2026-11-20 对齐 KitePlayer 工具链: compileSdk 37 + Kotlin 2.4.21 + Compose 1.13
    // API 37 官方要求 AGP >= 9.1.1; 选 AGP 9.3.0 + Gradle 9.5.0, 落在
    // Kotlin 2.4.21 的官方兼容区间(Gradle 7.6.3 ~ 9.5.0)内。JDK 17+(实测 JDK 21)
    id("com.android.application") version "9.3.0" apply false
    id("com.android.library") version "9.3.0" apply false
    id("org.jetbrains.kotlin.multiplatform") version "2.4.21" apply false
    id("org.jetbrains.kotlin.android") version "2.4.21" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.21" apply false
}
