package com.sbro.emucorer

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
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

    @Test fun appDoesNotRequestForegroundServicePermissions() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val permissions = context.packageManager.getPackageInfo(
            context.packageName, PackageManager.GET_PERMISSIONS
        ).requestedPermissions.orEmpty()
        assertFalse(permissions.any { it.startsWith("android.permission.FOREGROUND_SERVICE") })
    }
}
