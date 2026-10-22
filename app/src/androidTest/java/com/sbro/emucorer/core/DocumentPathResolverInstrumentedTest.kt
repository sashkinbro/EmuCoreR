package com.sbro.emucorer.core

import android.content.Intent
import android.provider.DocumentsContract
import androidx.core.net.toUri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class DocumentPathResolverInstrumentedTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val authority = "com.sbro.emucorer.test.documents"
    private val single = DocumentsContract.buildDocumentUri(authority, "root/track")
    private val tree = DocumentsContract.buildTreeDocumentUri(authority, "root")
    // A file-provider style URI has a display name but no SAF document ID.
    private val fileUri = "content://$authority/open/track".toUri()

    @Before
    @After
    fun releaseFixturePermissions() {
        context.contentResolver.persistedUriPermissions
            .filter { it.uri.authority == authority }
            .forEach { permission ->
                val flags = (if (permission.isReadPermission) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
                    (if (permission.isWritePermission) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
                context.contentResolver.releasePersistableUriPermission(permission.uri, flags)
            }
    }

    @Test
    fun singleDocumentGrantDoesNotCrashGameLaunchFallback() {
        persistGrant("grantSingleFixture", single)

        assertEquals(fileUri.toString(), DocumentPathResolver.prepareGameLaunchPath(context, fileUri.toString()))
    }

    @Test
    fun singleDocumentGrantIsNotUsedAsDirectoryAccess() {
        persistGrant("grantSingleFixture", single)

        assertNull(DocumentPathResolver.findAccessibleTreeUriForRawPath(context, "/storage/root/track"))
        assertNull(DocumentPathResolver.findAccessibleTreeUriForRawPath(context, "/storage/root/track/child.bin"))
    }

    @Test
    fun treeLookupStillWorksAlongsideSingleDocumentGrants() {
        persistGrant("grantSingleFixture", single)
        persistGrant("grantFixture", tree)

        assertEquals("/storage/root/track", DocumentPathResolver.resolveFilePath(context, fileUri.toString()))
        assertEquals(
            tree,
            DocumentPathResolver.findAccessibleTreeUriForRawPath(context, "/storage/root")
        )
    }

    private fun persistGrant(method: String, uri: android.net.Uri) {
        context.contentResolver.call(single, method, context.packageName, null)
        context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
}
