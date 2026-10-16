package com.sbro.emucorer.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MemoryCardNamingTest {

    // -------------------------------------------------------------- normalize

    @Test
    fun `plain name gets the card extension`() {
        assertEquals("Tekken 3.mcd", normalizeMemoryCardName("Tekken 3"))
    }

    @Test
    fun `an existing supported extension is replaced`() {
        assertEquals("save game.mcd", normalizeMemoryCardName("save game.mcd"))
        assertEquals("save game.mcd", normalizeMemoryCardName("save game.MCD"))
        assertEquals("save game.mcd", normalizeMemoryCardName("save game.mcr"))
        assertEquals("save game.mcd", normalizeMemoryCardName("save game.bin"))
    }

    @Test
    fun `dots inside the name are kept`() {
        assertEquals("v1.2 final.mcd", normalizeMemoryCardName("v1.2 final"))
    }

    @Test
    fun `unsafe characters become underscores`() {
        assertEquals("Tekken_ 3_.mcd", normalizeMemoryCardName("Tekken: 3!"))
        assertEquals("Crash_ Team Racing.mcd", normalizeMemoryCardName("Crash/ Team Racing"))
    }

    @Test
    fun `whitespace runs collapse into single spaces`() {
        assertEquals("Tekken 3.mcd", normalizeMemoryCardName("Tekken   3"))
    }

    @Test
    fun `leading and trailing dots and spaces are trimmed`() {
        assertEquals("hidden.mcd", normalizeMemoryCardName(" .hidden. "))
    }

    @Test
    fun `blank names fall back to the default label`() {
        assertEquals("Memory Card.mcd", normalizeMemoryCardName(""))
        assertEquals("Memory Card.mcd", normalizeMemoryCardName("   "))
        assertEquals("Memory Card.mcd", normalizeMemoryCardName("..."))
    }

    @Test
    fun `punctuation-only names survive as underscores`() {
        assertEquals("___.mcd", normalizeMemoryCardName("***"))
    }

    @Test
    fun `normalization is stable for an already normalized name`() {
        val once = normalizeMemoryCardName("SLES-12345")
        assertEquals(once, normalizeMemoryCardName(once))
    }

    // ------------------------------------------------------------- uniqueness

    @Test
    fun `a free name is returned unchanged`() {
        val name = pickUniqueMemoryCardName("Tekken 3.mcd", exists = { false })

        assertEquals("Tekken 3.mcd", name)
    }

    @Test
    fun `an occupied name receives a numeric suffix`() {
        val name = pickUniqueMemoryCardName("Tekken 3.mcd", exists = { it == "Tekken 3.mcd" })

        assertEquals("Tekken 3 (2).mcd", name)
    }

    @Test
    fun `the suffix keeps incrementing while names stay occupied`() {
        val taken = setOf("Tekken 3.mcd", "Tekken 3 (2).mcd")
        val name = pickUniqueMemoryCardName("Tekken 3.mcd", exists = { it in taken })

        assertEquals("Tekken 3 (3).mcd", name)
    }

    @Test
    fun `the same name counts as free while it belongs to the current card`() {
        val name = pickUniqueMemoryCardName(
            "Tekken 3.mcd",
            exists = { it == "Tekken 3.mcd" },
            currentName = "Tekken 3.mcd"
        )

        assertEquals("Tekken 3.mcd", name)
    }

    @Test
    fun `the current card name comparison ignores case`() {
        val name = pickUniqueMemoryCardName(
            "Tekken 3.mcd",
            exists = { it.equals("Tekken 3.MCD", ignoreCase = true) },
            currentName = "tekken 3.MCD"
        )

        assertEquals("Tekken 3.mcd", name)
    }

    @Test
    fun `uniqueness probes names that actually exist in the folder`() {
        val existing = setOf("A.mcd", "A (2).mcd", "A (3).mcd", "B.mcd")
        val probed = mutableListOf<String>()
        val name = pickUniqueMemoryCardName("A.mcd", exists = { candidate ->
            probed += candidate
            candidate in existing
        })

        assertEquals("A (4).mcd", name)
        assertTrue(probed.contains("A.mcd"))
        assertTrue(probed.contains("A (2).mcd"))
        assertTrue(probed.contains("A (3).mcd"))
        assertFalse(probed.contains("B.mcd"))
    }
}
