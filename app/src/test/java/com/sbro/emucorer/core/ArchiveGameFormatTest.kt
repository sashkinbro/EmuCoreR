package com.sbro.emucorer.core

import org.junit.Assert.assertTrue
import org.junit.Test

class ArchiveGameFormatTest {
    @Test
    fun archivesAreAcceptedAsGameImagesRegardlessOfExtensionCase() {
        for (name in listOf("game.zip", "game.7z", "game.rar", "GAME.ZIP", "GAME.7Z", "GAME.RAR")) {
            assertTrue("Archive should be launchable: $name", SetupValidator.isSupportedDiscImageName(name))
        }
    }
}
