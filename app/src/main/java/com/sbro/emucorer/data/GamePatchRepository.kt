package com.sbro.emucorer.data

import android.content.Context
import com.sbro.emucorer.core.EmulatorStorage
import java.io.File
import java.util.Locale

enum class GamePatchKind {
    WIDESCREEN,
    GENERAL
}

data class GamePatchEntry(
    val file: File,
    val kind: GamePatchKind,
    val aspectRatioOverride: String? = null,
    val disableWidescreenHack: Boolean = false,
    val author: String? = null
) {
    val hasDisplayDirectives: Boolean
        get() = aspectRatioOverride != null || disableWidescreenHack
}

class GamePatchRepository(context: Context) {
    private val appContext = context.applicationContext
    private val preferences = AppPreferences(appContext)
    private val cheatRepository = CheatRepository(appContext)

    fun patchesDirectory(): File =
        EmulatorStorage.patchesDir(appContext, preferences.getEmulatorDataPathSync())

    fun findPatchFiles(serial: String?, crc: String?): List<GamePatchEntry> {
        val serialKey = serial?.let(::compactKey)?.takeIf { it.length >= 8 }
        val crcKey = crc?.let(::compactKey)?.takeIf { it.length == 8 }
        if (serialKey == null && crcKey == null) return emptyList()
        return patchesDirectory()
            .listFiles { file ->
                file.isFile && file.extension.lowercase(Locale.US) in SUPPORTED_PATCH_EXTENSIONS
            }
            .orEmpty()
            .filter { file ->
                val key = compactKey(file.nameWithoutExtension)
                (serialKey != null && key.startsWith(serialKey)) ||
                    (crcKey != null && key.contains(crcKey))
            }
            .map { file -> entryFor(file) }
            .sortedBy { it.file.name.lowercase(Locale.US) }
    }

    private fun entryFor(file: File): GamePatchEntry {
        val header = runCatching {
            file.bufferedReader().use { reader ->
                buildString {
                    repeat(80) {
                        val line = reader.readLine() ?: return@repeat
                        appendLine(line)
                    }
                }
            }
        }.getOrDefault("")
        val aspect = Regex("(?im)^\\s*OverrideAspectRatio\\s*=\\s*(\\S+)")
            .find(header)?.groupValues?.get(1)?.trim()
        val disableHack = Regex("(?im)^\\s*DisableWidescreenRendering\\s*=\\s*(true|1)\\s*$")
            .containsMatchIn(header)
        val author = Regex("(?im)^\\s*Author\\s*=\\s*(.+)$")
            .find(header)?.groupValues?.get(1)?.trim()
        return GamePatchEntry(
            file = file,
            kind = classify(file.nameWithoutExtension),
            aspectRatioOverride = aspect,
            disableWidescreenHack = disableHack,
            author = author
        )
    }

    data class GamePatchDirectives(
        val aspectRatioOverride: String? = null,
        val disableWidescreenHack: Boolean = false,
        val author: String? = null
    )

    fun resolveDirectives(
        serial: String?,
        crc: String?,
        widescreen: Boolean
    ): GamePatchDirectives {
        if (!widescreen) return GamePatchDirectives()
        val entries = findPatchFiles(serial, crc).filter { entry ->
            when (entry.kind) {
                GamePatchKind.WIDESCREEN -> true
                // Unclassified files are display patches only when they declare
                // aspect-ratio directives; everything else in the database
                // (60 FPS, bug fixes, unused content) must not be auto-applied.
                GamePatchKind.GENERAL -> entry.hasDisplayDirectives
            }
        }
        return GamePatchDirectives(
            aspectRatioOverride = entries.firstNotNullOfOrNull { it.aspectRatioOverride },
            disableWidescreenHack = entries.any { it.disableWidescreenHack },
            author = entries.firstNotNullOfOrNull { it.author }
        )
    }

    fun buildPatchBlocks(
        serial: String?,
        crc: String?,
        widescreen: Boolean
    ): List<CheatBlock> {
        if (!widescreen) return emptyList()
        val entries = findPatchFiles(serial, crc).filter { entry ->
            when (entry.kind) {
                GamePatchKind.WIDESCREEN -> true
                GamePatchKind.GENERAL -> entry.hasDisplayDirectives
            }
        }
        val merged = linkedMapOf<String, CheatBlock>()
        entries.forEach { entry ->
            val blocks = runCatching { cheatRepository.parsePatchBlocks(entry.file.readText()) }
                .getOrDefault(emptyList())
            blocks.forEach { block ->
                val signature = block.title.trim().lowercase(Locale.US) + "\u0000" +
                    block.author?.trim().orEmpty() + "\u0000" +
                    block.lines.joinToString("\n")
                merged.putIfAbsent(signature, block)
            }
        }
        return merged.values.toList()
    }

    private fun classify(baseName: String): GamePatchKind {
        val tokens = baseName.lowercase(Locale.US).split(Regex("[^a-z0-9]+")).toSet()
        val widescreen = tokens.any { it == "ws" || it == "wide" || it == "widescreen" } ||
            baseName.contains("widescreen", ignoreCase = true)
        return if (widescreen) GamePatchKind.WIDESCREEN else GamePatchKind.GENERAL
    }

    private fun compactKey(value: String): String =
        value.uppercase(Locale.US).replace(Regex("[^A-Z0-9]"), "")

    companion object {
        private val SUPPORTED_PATCH_EXTENSIONS = setOf("pnach", "cht")
    }
}
