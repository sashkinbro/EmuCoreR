package com.sbro.emucorer.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ArchiveGameFormatTest {
    @Test
    fun archivesAreAcceptedAsGameImagesRegardlessOfExtensionCase() {
        for (name in listOf("game.zip", "game.7z", "game.rar", "GAME.ZIP", "GAME.7Z", "GAME.RAR")) {
            assertTrue("Archive should be launchable: $name", SetupValidator.isSupportedDiscImageName(name))
        }
    }

    @Test
    fun archiveExtensionsAreRemovedBeforeTitleAndCoverLookup() {
        assertEquals("Doom", parsePs1MetadataFromName("Doom (USA).zip").title)
        assertEquals("Silent Hill", parsePs1MetadataFromName("Silent Hill (USA).7z").title)
        assertEquals("Tekken 3", parsePs1MetadataFromName("Tekken 3 (USA).rar").title)
    }

    @Test
    fun serialIsStillRecoveredFromAnArchiveFileName() {
        val metadata = parsePs1MetadataFromName("Tekken 3 (USA) [SLUS-00402].zip")
        assertEquals("SLUS-00402", metadata.serial)
        assertEquals("Tekken 3", metadata.title)
    }

    @Test
    fun unknownDotsAreNotMistakenForAContainerExtension() {
        assertEquals("Rayman 2.0", parsePs1MetadataFromName("Rayman 2.0").title)
    }
}
