package com.sbro.emucorer

import android.content.ComponentName
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorer.network.RemotePlayCaptureService
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AppManifestInstrumentedTest {
    @Test fun fileUrisResolveToTheGameLauncher() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val intent = Intent(Intent.ACTION_VIEW, Uri.parse("file:///sdcard/games/Tekken.cue"))
            .addCategory(Intent.CATEGORY_BROWSABLE)
            .setPackage(context.packageName)
        val activity = context.packageManager.resolveActivity(intent, 0)
        assertEquals(MainActivity::class.java.name, activity?.activityInfo?.name)
    }

    @Test fun screenCaptureServiceDeclaresItsForegroundType() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val service = context.packageManager.getServiceInfo(
            ComponentName(context, RemotePlayCaptureService::class.java), 0
        )
        assertFalse(service.exported)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            assertEquals(ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION, service.foregroundServiceType)
        }
    }
}
