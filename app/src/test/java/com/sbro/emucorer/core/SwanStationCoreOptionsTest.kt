package com.sbro.emucorer.core

import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SwanStationCoreOptionsTest {

    private val cardTypeKeys = listOf(
        "swanstation_MemoryCards_Card1Type",
        "swanstation_MemoryCards_Card2Type"
    )

    @Test
    fun `memory card type keys are managed by the app`() {
        cardTypeKeys.forEach { key ->
            assertTrue("$key must be managed", SwanStationCoreOptions.isManagedKey(key))
        }
        assertTrue(
            SwanStationCoreOptions.isManagedKey("swanstation_MemoryCards_UsePlaylistTitle")
        )
    }

    @Test
    fun `memory card type keys stay in the catalogue for the core bridge`() {
        cardTypeKeys.forEach { key ->
            assertNotNull("$key must stay in the catalogue", SwanStationCoreOptions.option(key))
        }
    }

    @Test
    fun `rendered option lists never offer memory card types`() {
        val renderedLists = listOf(
            SwanStationCoreOptions.controlsOptions(),
            SwanStationCoreOptions.graphicsOptions(),
            SwanStationCoreOptions.emulationOptions(),
            SwanStationCoreOptions.audioOptions(),
            SwanStationCoreOptions.gameMenuControlsOptions(),
            SwanStationCoreOptions.gameMenuGraphicsOptions()
        )

        renderedLists.forEach { options ->
            options.forEach { option ->
                assertFalse(
                    "memory card option must not be rendered: ${option.key}",
                    option.key in cardTypeKeys || option.key == "swanstation_MemoryCards_UsePlaylistTitle"
                )
            }
        }
    }

    @Test
    fun `the catalogue no longer exposes any rendered memory card entry`() {
        val renderedKeys = listOf(
            SwanStationCoreOptions.controlsOptions(),
            SwanStationCoreOptions.graphicsOptions(),
            SwanStationCoreOptions.emulationOptions(),
            SwanStationCoreOptions.audioOptions()
        ).flatten().map { it.key }

        assertTrue(renderedKeys.none { it.startsWith("swanstation_MemoryCards") })
    }
}
