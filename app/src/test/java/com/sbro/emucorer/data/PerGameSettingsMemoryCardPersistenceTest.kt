package com.sbro.emucorer.data

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PerGameSettingsMemoryCardPersistenceTest {

    private fun baseProfile(vararg overrides: Pair<String, Any?>): PerGameSettings {
        var profile = PerGameSettings(gameKey = "tekken.cue", gameTitle = "Tekken 3")
        overrides.forEach { (key, value) ->
            profile = when (key) {
                "memoryCardSlot1" -> profile.copy(memoryCardSlot1 = value as String?)
                "memoryCardSlot2" -> profile.copy(memoryCardSlot2 = value as String?)
                "dedicatedMemoryCards" -> profile.copy(dedicatedMemoryCards = value as List<String>)
                "providedKeys" -> profile.copy(providedKeys = value as Set<String>?)
                "coreOptions" -> profile.copy(coreOptions = value as Map<String, String>)
                else -> error("Unsupported override key: $key")
            }
        }
        return profile
    }

    @Test
    fun `profiles without card data default to following the global assignment`() {
        val parsed = JSONObject("""{"gameKey":"tekken.cue","gameTitle":"Tekken 3"}""")
            .toPerGameSettings()

        assertNull(parsed.memoryCardSlot1)
        assertNull(parsed.memoryCardSlot2)
        assertEquals(emptyList<String>(), parsed.dedicatedMemoryCards)
    }

    @Test
    fun `card overrides survive a full JSON round trip`() {
        val profile = baseProfile(
            "memoryCardSlot1" to "Tekken 3.mcd",
            "memoryCardSlot2" to "MemoryCard2.mcd",
            "dedicatedMemoryCards" to listOf("Tekken 3.mcd")
        )

        val parsed = profile.toJson().toPerGameSettings()

        assertEquals("Tekken 3.mcd", parsed.memoryCardSlot1)
        assertEquals("MemoryCard2.mcd", parsed.memoryCardSlot2)
        assertEquals(listOf("Tekken 3.mcd"), parsed.dedicatedMemoryCards)
    }

    @Test
    fun `an explicit empty slot survives the round trip as no card`() {
        val profile = baseProfile("memoryCardSlot1" to "")

        val parsed = profile.toJson().toPerGameSettings()

        assertEquals("", parsed.memoryCardSlot1)
    }

    @Test
    fun `global assignments are not written into the profile`() {
        val profile = baseProfile(
            "memoryCardSlot1" to null,
            "memoryCardSlot2" to null,
            "dedicatedMemoryCards" to emptyList<String>()
        )

        val json = profile.toJson()

        assertFalse(json.has("memoryCardSlot1"))
        assertFalse(json.has("memoryCardSlot2"))
        assertFalse(json.has("dedicatedMemoryCards"))
    }

    @Test
    fun `partial profiles only write the keys they provide`() {
        val profile = baseProfile(
            "providedKeys" to setOf("coreOptions"),
            "memoryCardSlot1" to "Tekken 3.mcd",
            "coreOptions" to mapOf("swanstation_GPU_TextureFilter" to "xBR")
        )

        val json = profile.toJson()

        assertFalse(json.has("memoryCardSlot1"))
        assertTrue(json.has("coreOptions"))
    }

    @Test
    fun `legacy No-card type migrates to an empty slot and is stripped`() {
        val parsed = JSONObject(
            """
            {
              "gameKey": "tekken.cue",
              "gameTitle": "Tekken 3",
              "coreOptions": {
                "swanstation_MemoryCards_Card1Type": "None",
                "swanstation_MemoryCards_Card2Type": "NonPersistent",
                "swanstation_GPU_TextureFilter": "xBR"
              }
            }
            """.trimIndent()
        ).toPerGameSettings()

        assertEquals("", parsed.memoryCardSlot1)
        assertEquals("", parsed.memoryCardSlot2)
        assertEquals(mapOf("swanstation_GPU_TextureFilter" to "xBR"), parsed.coreOptions)
    }

    @Test
    fun `legacy file types migrate to the global assignment and are stripped`() {
        for (legacyType in listOf("Libretro", "Shared", "PerGame", "PerGameTitle", "PerGameFileTitle")) {
            val parsed = JSONObject(
                """
                {
                  "gameKey": "tekken.cue",
                  "gameTitle": "Tekken 3",
                  "coreOptions": {"swanstation_MemoryCards_Card1Type": "$legacyType"}
                }
                """.trimIndent()
            ).toPerGameSettings()

            assertNull("legacy type $legacyType must fall back to global", parsed.memoryCardSlot1)
            assertFalse(parsed.coreOptions.containsKey("swanstation_MemoryCards_Card1Type"))
        }
    }

    @Test
    fun `an explicit card override wins over a legacy type`() {
        val parsed = JSONObject(
            """
            {
              "gameKey": "tekken.cue",
              "gameTitle": "Tekken 3",
              "memoryCardSlot1": "Tekken 3.mcd",
              "coreOptions": {"swanstation_MemoryCards_Card1Type": "None"}
            }
            """.trimIndent()
        ).toPerGameSettings()

        assertEquals("Tekken 3.mcd", parsed.memoryCardSlot1)
        assertFalse(parsed.coreOptions.containsKey("swanstation_MemoryCards_Card1Type"))
    }

    @Test
    fun `a migrated legacy none is written back as the empty slot`() {
        val parsed = JSONObject(
            """
            {
              "gameKey": "tekken.cue",
              "gameTitle": "Tekken 3",
              "coreOptions": {"swanstation_MemoryCards_Card1Type": "None"}
            }
            """.trimIndent()
        ).toPerGameSettings()

        val rewritten = parsed.copy(providedKeys = null).toJson()

        assertEquals("", rewritten.getString("memoryCardSlot1"))
        val rewrittenCoreOptions = rewritten.optJSONObject("coreOptions")
        assertTrue(
            rewrittenCoreOptions == null || !rewrittenCoreOptions.has("swanstation_MemoryCards_Card1Type")
        )
    }

    @Test
    fun `blank entries in the dedicated card list are dropped`() {
        val parsed = JSONObject(
            """
            {
              "gameKey": "tekken.cue",
              "gameTitle": "Tekken 3",
              "dedicatedMemoryCards": ["Tekken 3.mcd", "", "  "]
            }
            """.trimIndent()
        ).toPerGameSettings()

        assertEquals(listOf("Tekken 3.mcd"), parsed.dedicatedMemoryCards)
    }

    @Test
    fun `legacy type keys never leak back into the written core options`() {
        val profile = baseProfile(
            "coreOptions" to mapOf(
                "swanstation_MemoryCards_Card1Type" to "Libretro",
                "swanstation_MemoryCards_Card2Type" to "Shared",
                "swanstation_GPU_TextureFilter" to "Nearest"
            )
        )

        val json = profile.toJson().getJSONObject("coreOptions")

        assertFalse(json.has("swanstation_MemoryCards_Card1Type"))
        assertFalse(json.has("swanstation_MemoryCards_Card2Type"))
        assertEquals("Nearest", json.getString("swanstation_GPU_TextureFilter"))
    }
}
