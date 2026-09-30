package com.sbro.emucorer.core

import org.junit.Assert.assertEquals
import org.junit.Test

class AndroidNativeCoreSelectorTest {

    @Test
    fun `standard page size uses the standard core`() {
        assertEquals("emucorer_jni", selectAndroidNativeCoreLibrary(4_096L))
    }

    @Test
    fun `large page size uses the 16 KiB core`() {
        assertEquals("emucorer_jni_16k", selectAndroidNativeCoreLibrary(16_384L))
    }

    @Test(expected = IllegalStateException::class)
    fun `unsupported page size is rejected`() {
        selectAndroidNativeCoreLibrary(65_536L)
    }
}
