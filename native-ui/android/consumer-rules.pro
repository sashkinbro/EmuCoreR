# Consumer R8/ProGuard rules for the :native-ui:android module (libemucorer_ps1ui.so).
# JNI_OnLoad in ps1_jni.cpp resolves these classes and their @JvmStatic methods by name,
# and Ps1ShellNative exports external native entry points.
-keep class com.sbro.emucorer.ps1ui.Ps1ShellNative { *; }
-keep class com.sbro.emucorer.ps1ui.Ps1ShellHost { *; }
-keep class com.sbro.emucorer.ps1ui.Ps1TextRasterizer { *; }
-keep class com.sbro.emucorer.ps1ui.Ps1ImageDecoder { *; }
-keep class com.sbro.emucorer.ps1ui.Ps1ShellAudio { *; }
-keep class com.sbro.emucorer.ps1ui.Ps1ShellSurfaceRenderer { *; }
