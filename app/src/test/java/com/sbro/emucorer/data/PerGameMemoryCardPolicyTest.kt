package com.sbro.emucorer.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PerGameMemoryCardPolicyTest {

    private fun card(name: String): MemoryCardInfo = MemoryCardInfo(
        name = name,
        path = "/data/memcards/$name",
        modifiedTime = 0L,
        type = MEMORY_CARD_TYPE_FILE,
        fileType = MEMORY_CARD_FILE_TYPE_PS1,
        sizeBytes = PS1_MEMORY_CARD_SIZE_BYTES,
        formatted = true,
        isDefaultCard = name.equals("MemoryCard1.mcd", true) || name.equals("MemoryCard2.mcd", true)
    )

    private val cards = listOf(
        card("MemoryCard1.mcd"),
        card("SLES-12345.mcd"),
        card("Tekken 3.mcd")
    )

    // ------------------------------------------------------------------ resolve

    @Test
    fun `full profile override wins over the global assignment`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = null,
            key = "memoryCardSlot1",
            globalCard = "MemoryCard1.mcd",
            override = "Tekken 3.mcd"
        )

        assertEquals("Tekken 3.mcd", resolved)
    }

    @Test
    fun `full profile without override follows the global assignment`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = null,
            key = "memoryCardSlot1",
            globalCard = "MemoryCard1.mcd",
            override = null
        )

        assertEquals("MemoryCard1.mcd", resolved)
    }

    @Test
    fun `empty override disables the slot even with a global card`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = null,
            key = "memoryCardSlot1",
            globalCard = "MemoryCard1.mcd",
            override = ""
        )

        assertNull(resolved)
    }

    @Test
    fun `partial profile without the card key ignores its stored override`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = setOf("coreOptions"),
            key = "memoryCardSlot1",
            globalCard = "MemoryCard1.mcd",
            override = "Tekken 3.mcd"
        )

        assertEquals("MemoryCard1.mcd", resolved)
    }

    @Test
    fun `partial profile that provided the card key applies its override`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = setOf("coreOptions", "memoryCardSlot1"),
            key = "memoryCardSlot1",
            globalCard = "MemoryCard1.mcd",
            override = "Tekken 3.mcd"
        )

        assertEquals("Tekken 3.mcd", resolved)
    }

    @Test
    fun `partial profile that provided the card key can still disable the slot`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = setOf("memoryCardSlot1"),
            key = "memoryCardSlot1",
            globalCard = "MemoryCard1.mcd",
            override = ""
        )

        assertNull(resolved)
    }

    @Test
    fun `no global card and no override stays empty`() {
        val resolved = resolvePerGameMemoryCardOverride(
            providedKeys = null,
            key = "memoryCardSlot2",
            globalCard = null,
            override = null
        )

        assertNull(resolved)
    }

    // ----------------------------------------------------------------- validate

    @Test
    fun `null selection stays empty`() {
        assertNull(validatePerGameMemoryCardName(null, "MemoryCard1.mcd", setOf("MemoryCard1.mcd")))
    }

    @Test
    fun `empty selection stays empty instead of resurrecting the fallback`() {
        assertNull(validatePerGameMemoryCardName("", "MemoryCard1.mcd", setOf("MemoryCard1.mcd")))
    }

    @Test
    fun `existing selection is kept`() {
        val validated = validatePerGameMemoryCardName(
            selected = "Tekken 3.mcd",
            fallback = "MemoryCard1.mcd",
            availableCards = setOf("MemoryCard1.mcd", "Tekken 3.mcd")
        )

        assertEquals("Tekken 3.mcd", validated)
    }

    @Test
    fun `stale selection falls back to the global assignment`() {
        val validated = validatePerGameMemoryCardName(
            selected = "Deleted Card.mcd",
            fallback = "MemoryCard1.mcd",
            availableCards = setOf("MemoryCard1.mcd")
        )

        assertEquals("MemoryCard1.mcd", validated)
    }

    @Test
    fun `stale selection without fallback becomes empty`() {
        val validated = validatePerGameMemoryCardName(
            selected = "Deleted Card.mcd",
            fallback = null,
            availableCards = emptySet()
        )

        assertNull(validated)
    }

    // ------------------------------------------------------------- option ids

    @Test
    fun `option id maps the tri-state value`() {
        assertEquals(MEMORY_CARD_OPTION_GLOBAL, memoryCardOptionFor(null, cards))
        assertEquals(MEMORY_CARD_OPTION_NONE, memoryCardOptionFor("", cards))
        assertEquals(2, memoryCardOptionFor("SLES-12345.mcd", cards))
        assertEquals(3, memoryCardOptionFor("Tekken 3.mcd", cards))
    }

    @Test
    fun `option id matches card names case-insensitively`() {
        assertEquals(3, memoryCardOptionFor("tekken 3.MCD", cards))
    }

    @Test
    fun `unknown card name falls back to the global option`() {
        assertEquals(MEMORY_CARD_OPTION_GLOBAL, memoryCardOptionFor("Missing.mcd", cards))
    }

    @Test
    fun `option id maps back to the stored tri-state value`() {
        assertNull(memoryCardSelectionForOption(MEMORY_CARD_OPTION_GLOBAL, cards))
        assertNull(memoryCardSelectionForOption(MEMORY_CARD_OPTION_CREATE, cards))
        assertEquals("", memoryCardSelectionForOption(MEMORY_CARD_OPTION_NONE, cards))
        assertEquals("SLES-12345.mcd", memoryCardSelectionForOption(2, cards))
        assertNull(memoryCardSelectionForOption(99, cards))
    }

    @Test
    fun `every card round-trips through the option mapping`() {
        cards.forEach { card ->
            val option = memoryCardOptionFor(card.name, cards)
            assertEquals(card.name, memoryCardSelectionForOption(option, cards))
        }
    }

    // ---------------------------------------------------------- foreign cards

    @Test
    fun `foreign dedicated cards exclude the current game and are lowercased`() {
        val profiles = listOf(
            PerGameSettings(
                gameKey = "tekken.cue",
                gameTitle = "Tekken 3",
                dedicatedMemoryCards = listOf("Tekken 3.mcd")
            ),
            PerGameSettings(
                gameKey = "crash.cue",
                gameTitle = "Crash",
                dedicatedMemoryCards = listOf("Crash 2.mcd", "Crash 3.mcd")
            )
        )

        val foreign = foreignDedicatedCardNames(profiles, currentGameKey = "tekken.cue")

        assertEquals(setOf("crash 2.mcd", "crash 3.mcd"), foreign)
    }

    @Test
    fun `foreign dedicated cards ignore profiles without dedicated cards`() {
        val profiles = listOf(
            PerGameSettings(gameKey = "a.cue", gameTitle = "A"),
            PerGameSettings(gameKey = "b.cue", gameTitle = "B")
        )

        assertTrue(foreignDedicatedCardNames(profiles, "a.cue").isEmpty())
    }

    @Test
    fun `create option is offered while the game has no live dedicated card`() {
        assertTrue(canCreateDedicatedCard(emptyList(), cards))
        assertTrue(canCreateDedicatedCard(listOf("Gone.mcd"), cards))
    }

    @Test
    fun `create option is hidden once the dedicated card exists`() {
        assertFalse(canCreateDedicatedCard(listOf("Tekken 3.mcd"), cards))
        assertFalse(canCreateDedicatedCard(listOf("tekken 3.MCD"), cards))
    }

    // ------------------------------------------------------------ base names

    @Test
    fun `card base name prefers the serial`() {
        assertEquals("SLES-12345", perGameCardBaseName("SLES-12345", "Tekken 3"))
    }

    @Test
    fun `card base name falls back to the title when the serial is missing`() {
        assertEquals("Tekken 3", perGameCardBaseName(null, "Tekken 3"))
        assertEquals("Tekken 3", perGameCardBaseName("   ", "Tekken 3"))
    }

    // ------------------------------------------------------------- migration

    @Test
    fun `legacy no-card types keep the slot empty`() {
        assertEquals("", legacyMemoryCardSlotOverride("None"))
        assertEquals("", legacyMemoryCardSlotOverride("NonPersistent"))
    }

    @Test
    fun `legacy file types fall back to the global assignment`() {
        assertNull(legacyMemoryCardSlotOverride(null))
        assertNull(legacyMemoryCardSlotOverride("Shared"))
        assertNull(legacyMemoryCardSlotOverride("Libretro"))
        assertNull(legacyMemoryCardSlotOverride("PerGame"))
        assertNull(legacyMemoryCardSlotOverride("PerGameTitle"))
        assertNull(legacyMemoryCardSlotOverride("PerGameFileTitle"))
        assertNull(legacyMemoryCardSlotOverride("SomethingElse"))
    }

    // -------------------------------------------------------------- card type

    @Test
    fun `a bound card path forces the shared core type`() {
        assertEquals("Shared", swanStationCardTypeForPath("/data/memcards/MemoryCard1.mcd"))
    }

    @Test
    fun `an empty slot forces the no-card core type`() {
        assertEquals("None", swanStationCardTypeForPath(null))
    }
}
