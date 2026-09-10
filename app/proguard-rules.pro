# EmuCoreR R8 rules.
#
# This app is a Compose/Kotlin frontend around the bundled PS1 core JNI
# library (libemucorer_jni.so). Only runtime name lookups need explicit keeps:
# JNI entry points, classes resolved by the Discord partner SDK, WebRTC's Java
# peers, and kotlinx.serialization serializers used for persisted JSON and
# typed Navigation Compose routes. Everything else is shrunk normally.

# Keep readable crash reports.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# Signatures/annotations are read at runtime by Room and the Discord SDK.
-keepattributes Signature,*Annotation*,InnerClasses,EnclosingMethod

# --- JNI --------------------------------------------------------------------
# Native entry points are resolved by their mangled names
# (Java_com_sbro_emucorer_core_NativeCoreBridge_*,
#  Java_com_sbro_emucorer_discord_DiscordNative_*), so class and method names
# must stay stable in release builds.
-keepclasseswithmembers,includedescriptorclasses class com.sbro.emucorer.core.NativeCoreBridge {
    native <methods>;
}
-keepclasseswithmembers,includedescriptorclasses class com.sbro.emucorer.discord.DiscordNative {
    native <methods>;
}

# --- Discord Social SDK -----------------------------------------------------
# libdiscord_partner_sdk.so instantiates its Java models and activity classes
# by name.
-keep class com.discord.socialsdk.** { *; }

# --- WebRTC -----------------------------------------------------------------
# The WebRTC native library resolves Java peers and callbacks through JNI and
# jni_zero, including classes that have no direct Kotlin references.
-keep class org.webrtc.** { *; }
-keep class org.jni_zero.** { *; }
# JniZeroJni is provided by WebRTC's own native build, not by the Java AAR.
-dontwarn org.jni_zero.JniZeroJni

# --- kotlinx.serialization --------------------------------------------------
# Persisted JSON models and typed Navigation routes decode through generated
# serializers/companions that R8 cannot see statically.
-keepclassmembers class com.sbro.emucorer.** {
    *** Companion;
}
-keepclasseswithmembers class com.sbro.emucorer.** {
    kotlinx.serialization.KSerializer serializer(...);
}
-keep,includedescriptorclasses class com.sbro.emucorer.**$$serializer { *; }
-keepclassmembers class **$$serializer {
    public static ** INSTANCE;
}
