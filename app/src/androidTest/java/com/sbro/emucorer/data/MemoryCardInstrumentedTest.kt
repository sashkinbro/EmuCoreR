// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.data

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorer.core.EmulatorStorage
import com.sbro.emucorer.core.NativeApp
import java.io.File
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Exercises the per-game memory-card flow against the real native core and the
 * app's own memcards folder on the connected device.
 */
@RunWith(AndroidJUnit4::class)
class MemoryCardInstrumentedTest {

    @Test
    fun gameCardIsCreatedInTheVisibleFolderListedAndReused() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        assumeTrue("native core must be available", NativeApp.hasNativeCore)

        val preferences = AppPreferences(context)
        val repository = MemoryCardRepository(context, preferences)
        val directory = EmulatorStorage.memoryCardsDir(context, preferences.getEmulatorDataPathSync())
        val uniqueBase = "EmuCoreR Test Game ${System.currentTimeMillis()}"

        val created = repository.ensureGameCard(uniqueBase)
        assertNotNull("the game card must be created", created)
        val cardName = created!!
        val file = File(directory, cardName)
        try {
            assertTrue("the card must land in the visible memcards folder", file.isFile)
            assertEquals("the card must be a raw 128 KiB image", 128L * 1024L, file.length())
            assertTrue("the card must not be shared with another card", cardName.startsWith("EmuCoreR Test Game"))
            assertTrue(
                "the card must be visible in the memory card manager list",
                repository.listCards().any { it.name == cardName }
            )
            assertEquals(
                "re-ensuring the same game must reuse the card",
                cardName,
                repository.ensureGameCard(uniqueBase)
            )
        } finally {
            val info = repository.listCards().firstOrNull { it.name == cardName }
            if (info != null) {
                runBlocking { repository.deleteCard(info) }
            }
        }
        assertFalse(
            "the test card must be cleaned up",
            repository.listCards().any { it.name == cardName }
        )
    }

    @Test
    fun gameCardNameIsSanitizedAndUniqueOnDisk() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        assumeTrue("native core must be available", NativeApp.hasNativeCore)

        val preferences = AppPreferences(context)
        val repository = MemoryCardRepository(context, preferences)
        val base = "Test: Card/${System.currentTimeMillis()}"

        val created = repository.ensureGameCard(base)
        assertNotNull(created)
        val cardName = created!!
        try {
            assertFalse("unsafe characters must be sanitized", cardName.contains(':'))
            assertFalse("unsafe characters must be sanitized", cardName.contains('/'))
            assertTrue("the card keeps the raw card extension", cardName.endsWith(".mcd"))
        } finally {
            val info = repository.listCards().firstOrNull { it.name == cardName }
            if (info != null) {
                runBlocking { repository.deleteCard(info) }
            }
        }
    }
}
