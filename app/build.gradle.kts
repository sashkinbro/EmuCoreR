import java.security.MessageDigest
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.google.services)
    alias(libs.plugins.ksp)
}

val localProperties = Properties().apply {
    val propertiesFile = rootProject.file("local.properties")
    if (propertiesFile.isFile) {
        propertiesFile.inputStream().use(::load)
    }
}

fun localProperty(name: String): String? = localProperties.getProperty(name)?.takeIf { it.isNotBlank() }

fun buildConfigString(value: String): String = "\"" + value
    .replace("\\", "\\\\")
    .replace("\"", "\\\"") + "\""

fun sha256Hex(value: String): String = MessageDigest.getInstance("SHA-256")
    .digest(value.toByteArray(Charsets.UTF_8))
    .joinToString("") { byte -> "%02x".format(byte.toInt() and 0xFF) }

val feedbackEndpoint = localProperty("emucorex.feedback.endpoint").orEmpty()
val feedbackApiKey = localProperty("emucorex.feedback.apiKey").orEmpty()
val emucorerCmakeVersion = "3.30.5"
val emucorerNdkVersion = "29.0.14206865"
val featureKey = localProperty("emucorex.features.key").orEmpty()
val featureKeyDigest = localProperty("emucorex.features.key")
    ?.let { sha256Hex("emucorex-features-v1:$it") }
    .orEmpty()
val catalogWorkerUrl = localProperty("emucorex.catalog.workerUrl")
    ?: "https://emucorex-catalog.kyivstar19971502.workers.dev"
val multiplayerSignalingUrl = "https://emucorex-multiplayer.kyivstar19971502.workers.dev"
val multiplayerClientCode = "ecx-mp-v1-7H4K9M2Q"
val discordApplicationId = "1536775623287115786"
val discordSdkDirectory = (
    providers.gradleProperty("emucorex.discord.sdkDir").orNull
        ?: localProperty("emucorex.discord.sdkDir")
        ?: providers.environmentVariable("DISCORD_SDK_DIR").orNull
    )
    ?.let(::file)
    ?.takeIf { sdkDir ->
        sdkDir.resolve("include/discordpp.h").isFile &&
            sdkDir.resolve("arm64-v8a/libdiscord_partner_sdk.so").isFile &&
            sdkDir.resolve("discord_partner_sdk.aar").isFile
    }

val releaseStoreFilePath = localProperty("emucorex.release.storeFile")
val releaseStorePassword = localProperty("emucorex.release.storePassword")
val releaseKeyAlias = localProperty("emucorex.release.keyAlias")
val releaseKeyPassword = localProperty("emucorex.release.keyPassword")
val releaseSigningConfigured = listOf(
    releaseStoreFilePath,
    releaseStorePassword,
    releaseKeyAlias,
    releaseKeyPassword
).all { it != null }

android {
    namespace = "com.sbro.emucorer"
    compileSdk {
        version = release(37)
    }
    ndkVersion = emucorerNdkVersion

    defaultConfig {
        applicationId = "com.sbro.emucorer"
        minSdk = 26
        targetSdk = 37
        versionCode = 30
        versionName = "0.0.9"

        buildConfigField("String", "FEEDBACK_ENDPOINT", buildConfigString(feedbackEndpoint))
        buildConfigField("String", "FEEDBACK_API_KEY", buildConfigString(feedbackApiKey))
        buildConfigField("String", "FEATURE_KEY", buildConfigString(featureKey))
        buildConfigField("String", "FEATURE_KEY_DIGEST", buildConfigString(featureKeyDigest))
        buildConfigField("String", "CATALOG_WORKER_URL", buildConfigString(catalogWorkerUrl))
        buildConfigField("String", "MULTIPLAYER_SIGNALING_URL", buildConfigString(multiplayerSignalingUrl))
        buildConfigField("String", "MULTIPLAYER_CLIENT_CODE", buildConfigString(multiplayerClientCode))
        buildConfigField("long", "DISCORD_APPLICATION_ID", "${discordApplicationId}L")
        buildConfigField("boolean", "DISCORD_SDK_AVAILABLE", (discordSdkDirectory != null).toString())
        manifestPlaceholders["discordSdkAvailable"] = (discordSdkDirectory != null).toString()

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            //noinspection ChromeOsAbiSupport
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++23"
                arguments += "-DANDROID_STL=c++_shared"
                discordSdkDirectory?.let { arguments += "-DDISCORD_SDK_DIR=${it.absolutePath}" }
            }
        }
    }

    signingConfigs {
        if (releaseSigningConfigured) {
            create("release") {
                storeFile = rootProject.file(releaseStoreFilePath!!)
                storePassword = releaseStorePassword!!
                keyAlias = releaseKeyAlias!!
                keyPassword = releaseKeyPassword!!
            }
        }
    }

    buildTypes {
        debug {
            // The emulator core is performance-sensitive even when the
            // Android frontend is debuggable. Keep symbols and assertions,
            // but do not run the complete native machine at Clang's implicit
            // -O0 when launching it from Android Studio.
            externalNativeBuild {
                cmake {
                    cFlags += "-O3"
                    cppFlags += "-O3"
                }
            }
        }
        release {
            if (releaseSigningConfigured) {
                signingConfig = signingConfigs.getByName("release")
            }
            // AGP configures CMake with CMAKE_BUILD_TYPE=RelWithDebInfo, whose
            // default flags are "-O2 -g -DNDEBUG" and are appended after any -O3
            // given through cFlags/cppFlags, so that -O2 would win. Replace the
            // RelWithDebInfo flags outright to keep symbols but compile the
            // emulator core at the same -O3 as the debug build.
            //
            // Overriding -DCMAKE_BUILD_TYPE=Release would also enable the
            // core's LTO block, but full LTO across the whole vendored machine
            // is a large build-time cost and stays a separate step.
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG",
                        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG"
                    )
                }
            }
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = emucorerCmakeVersion
        }
    }
    sourceSets {
        getByName("main") {
            // Populated by the secondary 16 KiB core build below.
            jniLibs.srcDir(file("build/generated/page-size-jni-libs"))
        }
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
            discordSdkDirectory?.let { pickFirsts += "**/libdiscord_partner_sdk.so" }
        }
    }
    discordSdkDirectory?.let { sdkDir -> sourceSets["main"].jniLibs.srcDir(sdkDir) }
    bundle {
        language {
            enableSplit = false
        }
    }
    lint {
        lintConfig = file("lint.xml")
    }
}

// SwanStation bakes the host page size into its fastmem implementation and code cache,
// so Android needs two native cores. AGP builds the normal 4 KiB core above; these tasks
// build the 16 KiB core in an isolated CMake tree and expose it as generated jniLibs
// before packaging, keeping the APK compatible with both page sizes.
val androidSdkPath = localProperty("sdk.dir")
    ?: providers.environmentVariable("ANDROID_SDK_ROOT").orNull
    ?: providers.environmentVariable("ANDROID_HOME").orNull
    ?: error("Android SDK path is missing. Set sdk.dir in local.properties.")
val androidSdkDirectory = file(androidSdkPath)
val androidNdkDirectory = androidSdkDirectory.resolve("ndk/$emucorerNdkVersion")
val hostExecutableSuffix = if (
    System.getProperty("os.name").startsWith("Windows", ignoreCase = true)
) ".exe" else ""
val cmakeExecutable = androidSdkDirectory.resolve(
    "cmake/$emucorerCmakeVersion/bin/cmake$hostExecutableSuffix"
)
val ninjaExecutable = androidSdkDirectory.resolve(
    "cmake/$emucorerCmakeVersion/bin/ninja$hostExecutableSuffix"
)
val secondary16kBuildDirectory = layout.buildDirectory.dir("native-secondary/16k/arm64-v8a")
val secondary16kObjectDirectory = layout.buildDirectory.dir("native-secondary/16k/obj/arm64-v8a")
val secondary16kCore = secondary16kObjectDirectory.map { it.file("libemucorer_jni_16k.so") }
val generated16kJniDirectory = layout.buildDirectory.dir("generated/page-size-jni-libs/arm64-v8a")

val configureEmucorer16k by tasks.registering(Exec::class) {
    group = "build"
    description = "Configures the secondary 16 KiB Android emulator core."
    inputs.files(fileTree("src/main/cpp") {
        include("**/CMakeLists.txt", "**/*.cmake")
    })
    inputs.files(fileTree("../../third_party/swanstation") {
        include("**/CMakeLists.txt", "**/*.cmake")
    })
    inputs.property("cmakeVersion", emucorerCmakeVersion)
    inputs.property("ndkVersion", emucorerNdkVersion)
    inputs.property("androidSdkPath", androidSdkDirectory.absolutePath)
    outputs.file(secondary16kBuildDirectory.map { it.file("CMakeCache.txt") })

    commandLine(
        cmakeExecutable.absolutePath,
        "-S", file("src/main/cpp").absolutePath,
        "-B", secondary16kBuildDirectory.get().asFile.absolutePath,
        "-G", "Ninja",
        "-DCMAKE_SYSTEM_NAME=Android",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_SYSTEM_VERSION=26",
        "-DANDROID_PLATFORM=android-26",
        "-DANDROID_ABI=arm64-v8a",
        "-DCMAKE_ANDROID_ARCH_ABI=arm64-v8a",
        "-DANDROID_NDK=${androidNdkDirectory.absolutePath}",
        "-DCMAKE_ANDROID_NDK=${androidNdkDirectory.absolutePath}",
        "-DCMAKE_TOOLCHAIN_FILE=${androidNdkDirectory.resolve("build/cmake/android.toolchain.cmake").absolutePath}",
        "-DCMAKE_MAKE_PROGRAM=${ninjaExecutable.absolutePath}",
        "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${secondary16kObjectDirectory.get().asFile.absolutePath}",
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${secondary16kObjectDirectory.get().asFile.absolutePath}",
        "-DANDROID=true",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DANDROID_STL=c++_shared",
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG",
        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG",
        "-DEMUCORER_HOST_PAGE_SIZE=16384",
        "-DEMUCORER_NATIVE_LIBRARY_NAME=emucorer_jni_16k"
    )
}

val buildEmucorer16k by tasks.registering(Exec::class) {
    group = "build"
    description = "Builds the secondary 16 KiB Android emulator core."
    dependsOn(configureEmucorer16k)
    outputs.file(secondary16kCore)
    outputs.upToDateWhen { false }
    commandLine(
        cmakeExecutable.absolutePath,
        "--build", secondary16kBuildDirectory.get().asFile.absolutePath,
        "--target", "emucorer_jni"
    )
    doLast {
        val coreFile = outputs.files.singleFile
        check(coreFile.isFile) {
            "The 16 KiB emulator core was not produced: $coreFile"
        }
    }
}

val stageEmucorer16k by tasks.registering(Copy::class) {
    group = "build"
    description = "Stages the 16 KiB core for standard APK and AAB packaging."
    dependsOn(buildEmucorer16k)
    from(secondary16kCore)
    into(generated16kJniDirectory)
}

tasks.matching { it.name == "mergeReleaseJniLibFolders" }.configureEach {
    dependsOn(stageEmucorer16k)
}

dependencies {
    implementation(libs.androidx.credentials)
    implementation(libs.androidx.credentials.play.services)
    implementation(libs.google.identity)
    implementation(libs.google.auth)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.fragment)
    implementation(libs.google.play.billing)
    implementation(libs.google.play.review)
    implementation(libs.google.play.review.ktx)
    implementation(libs.androidx.work.runtime)
    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.room.ktx)
    implementation(libs.coil.compose)
    implementation(libs.coil.network.okhttp)
    implementation(libs.androidx.browser)
    implementation(libs.webrtc.android)
    implementation(libs.android.youtube.player.core)
    ksp(libs.androidx.room.compiler)
    discordSdkDirectory?.let { sdkDir ->
        implementation(files(sdkDir.resolve("discord_partner_sdk.aar")))
    }
    implementation(platform(libs.firebase.bom))
    implementation(libs.firebase.analytics)
    implementation(libs.firebase.firestore)
    implementation(libs.firebase.auth)
    testImplementation(libs.junit)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}

ksp {
    arg("room.schemaLocation", "$projectDir/schemas")
}
