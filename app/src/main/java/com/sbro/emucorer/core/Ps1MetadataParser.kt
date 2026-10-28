package com.sbro.emucorer.core

import java.util.Locale

/**
 * Container/disc extensions that are not part of the game title. Archive
 * extensions belong here too: the library lists a `.zip`/`.7z`/`.rar` dump by
 * the game it contains, and the cleaned title is what drives the serial
 * reverse lookup and cover-art download.
 */
private val PS1_CONTAINER_EXTENSIONS = setOf(
    "iso", "bin", "cue", "img", "mdf", "gz", "cso", "zso", "chd", "elf",
    "zip", "7z", "rar", "pbp", "ecm", "mds", "exe", "psexe", "cpe", "m3u", "psf"
)

internal fun parsePs1MetadataFromName(rawName: String): GameMetadata {
    val trimmed = rawName.trim()
    val extension = trimmed.substringAfterLast('.', "").lowercase(Locale.ROOT)
    val cleanName = if (extension in PS1_CONTAINER_EXTENSIONS) {
        trimmed.substringBeforeLast('.').trim()
    } else {
        trimmed
    }
    val serial = extractPs1SerialFromName(cleanName)
    val title = cleanName
        .replace(Regex("""(?i)\b([A-Z]{4})[-_. ]?(\d{3})[-_. ]?(\d{2})\b"""), " ")
        .replace(Regex("""(?i)\b([A-Z]{4})[-_. ]?(\d{5})\b"""), " ")
        .replace(Regex("""\[[^]]*]|\([^)]*\)"""), " ")
        .replace(Regex("""\b(disc|disk|cd|dvd)\s*\d+\b""", RegexOption.IGNORE_CASE), " ")
        .replace('_', ' ')
        .replace(Regex("""\s+"""), " ")
        .trim()
        .ifBlank { cleanName }
    return GameMetadata(title = title, serial = serial, serialWithCrc = serial)
}

private fun extractPs1SerialFromName(value: String): String? {
    val normalized = value.uppercase(Locale.ROOT)
    val fullPattern = Regex("""\b([A-Z]{4})[-_. ]?(\d{3})[-_. ]?(\d{2})\b""")
    val compactPattern = Regex("""\b([A-Z]{4})[-_. ]?(\d{5})\b""")
    return fullPattern.find(normalized)?.let { match ->
        "${match.groupValues[1]}-${match.groupValues[2]}${match.groupValues[3]}"
    } ?: compactPattern.find(normalized)?.let { match ->
        "${match.groupValues[1]}-${match.groupValues[2]}"
    }
}
