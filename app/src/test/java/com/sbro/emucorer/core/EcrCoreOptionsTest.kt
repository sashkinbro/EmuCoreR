package com.sbro.emucorer.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EcrCoreOptionsTest {

    private val cardTypeKeys = listOf(
        "ecr_MemoryCards_Card1Type",
        "ecr_MemoryCards_Card2Type"
    )

    @Test
    fun `memory card type keys are managed by the app`() {
        cardTypeKeys.forEach { key ->
            assertTrue("$key must be managed", EcrCoreOptions.isManagedKey(key))
        }
        assertTrue(
            EcrCoreOptions.isManagedKey("ecr_MemoryCards_UsePlaylistTitle")
        )
    }

    @Test
    fun `memory card type keys stay in the catalogue for the core bridge`() {
        cardTypeKeys.forEach { key ->
            assertNotNull("$key must stay in the catalogue", EcrCoreOptions.option(key))
        }
    }

    @Test
    fun `rendered option lists never offer memory card types`() {
        val renderedLists = listOf(
            EcrCoreOptions.controlsOptions(),
            EcrCoreOptions.graphicsOptions(),
            EcrCoreOptions.emulationOptions(),
            EcrCoreOptions.audioOptions(),
            EcrCoreOptions.gameMenuControlsOptions(),
            EcrCoreOptions.gameMenuGraphicsOptions()
        )

        renderedLists.forEach { options ->
            options.forEach { option ->
                assertFalse(
                    "memory card option must not be rendered: ${option.key}",
                    option.key in cardTypeKeys || option.key == "ecr_MemoryCards_UsePlaylistTitle"
                )
            }
        }
    }

    @Test
    fun `the catalogue no longer exposes any rendered memory card entry`() {
        val renderedKeys = listOf(
            EcrCoreOptions.controlsOptions(),
            EcrCoreOptions.graphicsOptions(),
            EcrCoreOptions.emulationOptions(),
            EcrCoreOptions.audioOptions()
        ).flatten().map { it.key }

        assertTrue(renderedKeys.none { it.startsWith("ecr_MemoryCards") })
    }

    @Test
    fun `normalizeOptionKey rewrites legacy prefix while keeping ecr prefix unchanged`() {
        assertEquals("ecr_GPU_TextureFilter", EcrOptions.normalizeOptionKey("swanstation_GPU_TextureFilter"))
        assertEquals("ecr_CPU_ExecutionMode", EcrOptions.normalizeOptionKey("ecr_CPU_ExecutionMode"))
        assertEquals("other_key", EcrOptions.normalizeOptionKey("other_key"))
    }

    @Test
    fun `migrateEntriesMap copies legacy keys without overriding newer ecr values`() {
        val legacy = mapOf<String, Any?>(
            "swanstation_GPU_TextureFilter" to "xBR",
            "swanstation_CPU_ExecutionMode" to "Interpreter",
            "ignored_int" to 42,
            "__internal" to "skip"
        )
        val existingTarget = mapOf<String, Any?>(
            "ecr_GPU_TextureFilter" to "Nearest",
            "__migrated_from_swanstation" to true
        )

        val migrated = EcrOptions.migrateEntriesMap(legacy, existingTarget)

        assertEquals("Nearest", migrated["ecr_GPU_TextureFilter"])
        assertEquals("Interpreter", migrated["ecr_CPU_ExecutionMode"])
        assertFalse(migrated.containsKey("swanstation_GPU_TextureFilter"))
        assertFalse(migrated.containsKey("ignored_int"))
        assertFalse(migrated.containsKey("__internal"))
        assertFalse(migrated.containsKey("__migrated_from_swanstation"))
    }
}
