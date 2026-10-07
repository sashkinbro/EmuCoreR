package com.sbro.emucorer.data

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ActivePatchNoticeTest {

    @Test
    fun `patch notice reports anything only when something is active`() {
        val notice = ActivePatchNotice(
            widescreen = false,
            cheats = false,
            userPatchCount = 0
        )
        assertFalse(notice.hasAnything)
        assertTrue(notice.copy(userPatchCount = 2).hasAnything)
        assertTrue(notice.copy(widescreen = true).hasAnything)
        assertTrue(notice.copy(cheats = true).hasAnything)
    }
}
