package com.sbro.emucorer.data

import java.net.URI
import java.util.Locale

data class LibraryDisc(val number: Int, val game: GameItem)

private val discMarker = Regex("""(?<![\p{L}\p{N}])(?:disc|disk|cd)[\s._-]*(\d{1,2})(?:[\s._-]+of[\s._-]+\d{1,2})?(?![\p{L}\p{N}])""", RegexOption.IGNORE_CASE)
private fun withoutDiscMarker(value: String) = value.replace(discMarker, "")
    .replace(Regex("""\(\s*\)|\[\s*]"""), "")
    .replace(Regex("""\s+"""), " ").trim(' ', '-', '_', '.')

private data class DiscCandidate(val key: String, val disc: LibraryDisc)

private fun candidate(game: GameItem): DiscCandidate? {
    val stem = game.fileName.substringBeforeLast('.', game.fileName)
    val marker = discMarker.find(stem) ?: return null
    val number = marker.groupValues[1].toIntOrNull()?.takeIf { it > 0 } ?: return null
    val base = withoutDiscMarker(stem).lowercase(Locale.ROOT)
        .replace(Regex("""[^\p{L}\p{N}]+"""), " ").trim()
    if (base.isBlank()) return null
    val parent = if (game.path.startsWith("content://")) {
        val uri = runCatching { URI(game.path) }.getOrNull() ?: return null
        val document = uri.path.substringAfter("/document/", "")
        if ('/' !in document) return null
        "${uri.authority}:${document.substringBeforeLast('/')}"
    } else game.path.replace('\\', '/').substringBeforeLast('/', ".")
    return DiscCandidate("$parent\u0000$base", LibraryDisc(number, game))
}

/** Groups only explicitly numbered, unambiguous discs of the same edition and folder. */
internal fun groupLibraryDiscs(games: List<GameItem>, enabled: Boolean = true): List<GameItem> {
    if (!enabled) return games
    val candidates = games.mapNotNull(::candidate)
    val sets = candidates.groupBy { it.key }.filterValues { members ->
        members.size > 1 && members.map { it.disc.number }.distinct().size == members.size
    }.mapValues { (_, members) -> members.map { it.disc }.sortedBy { it.number } }
    val keysByPath = candidates.associate { it.disc.game.path to it.key }
    val published = HashSet<String>()
    return games.mapNotNull { game ->
        val key = keysByPath[game.path]
        val discs = sets[key] ?: return@mapNotNull game
        if (!published.add(key!!)) return@mapNotNull null
        val first = discs.first().game
        first.copy(
            title = withoutDiscMarker(first.title).ifBlank { first.title },
            fileSize = discs.sumOf { it.game.fileSize.coerceAtLeast(0L) },
            lastModified = discs.maxOf { it.game.lastModified },
            coverArtPath = discs.firstNotNullOfOrNull { it.game.coverArtPath },
            discs = discs
        )
    }
}
