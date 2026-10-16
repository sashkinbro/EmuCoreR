package com.sbro.emucorer.data

/**
 * Per-game memory-card policy.
 *
 * A profile stores each slot as a tri-state value:
 *  - `null` — follow the global slot assignment from the memory card manager,
 *  - empty string — no card in this slot,
 *  - any other value — the name of a card file inside the visible memcards
 *    folder.
 *
 * The stored value is resolved together with the card list so a game can pick
 * any card the manager shows, and a card created for one game can be hidden
 * from the other games' selectors.
 */

/** Selector option: use the global slot assignment. */
const val MEMORY_CARD_OPTION_GLOBAL = -1

/** Selector option: leave the slot empty. */
const val MEMORY_CARD_OPTION_NONE = 0

/** Selector option: create a new card owned by this game. */
const val MEMORY_CARD_OPTION_CREATE = -2

/** Per-game card names are capped before the extension is appended. */
const val GAME_CARD_NAME_MAX_LENGTH = 48

/**
 * Core options the app used to expose for memory-card types. They are owned by
 * the app now, so stored values are ignored and migrated on profile load.
 */
val LEGACY_MEMORY_CARD_CORE_OPTION_KEYS = setOf(
    "swanstation_MemoryCards_Card1Type",
    "swanstation_MemoryCards_Card2Type"
)

/**
 * The bundled SwanStation core only opens the bound memcards image in shared
 * mode; every other type (Libretro/PerGame/...) bypasses the path override.
 * A bound path therefore always maps to a shared card, an empty slot to none.
 */
fun swanStationCardTypeForPath(resolvedPath: String?): String =
    if (resolvedPath != null) "Shared" else "None"

/**
 * Resolves a per-game slot override against the global assignment. Partial
 * profiles (created by the in-game menu) only override keys they actually
 * stored; a full profile always wins.
 */
fun resolvePerGameMemoryCardOverride(
    providedKeys: Set<String>?,
    key: String,
    globalCard: String?,
    override: String?
): String? {
    if (providedKeys != null && key !in providedKeys) return globalCard
    return when {
        override == null -> globalCard
        override.isEmpty() -> null
        else -> override
    }
}

/**
 * Keeps a selected card while its file still exists in the memory card
 * manager; stale names (renamed or deleted cards) fall back to the global
 * assignment so a game never launches with a vanished card.
 */
fun validatePerGameMemoryCardName(
    selected: String?,
    fallback: String?,
    availableCards: Set<String>
): String? = when {
    selected == null || selected.isEmpty() -> null
    selected in availableCards -> selected
    else -> fallback
}

/** Maps the stored tri-state value to the selector option id. */
fun memoryCardOptionFor(
    selectedCardName: String?,
    cards: List<MemoryCardInfo>
): Int = when {
    selectedCardName == null -> MEMORY_CARD_OPTION_GLOBAL
    selectedCardName.isEmpty() -> MEMORY_CARD_OPTION_NONE
    else -> {
        val index = cards.indexOfFirst { it.name.equals(selectedCardName, ignoreCase = true) }
        if (index >= 0) index + 1 else MEMORY_CARD_OPTION_GLOBAL
    }
}

/** Maps a selector option id back to the stored tri-state value. */
fun memoryCardSelectionForOption(
    option: Int,
    cards: List<MemoryCardInfo>
): String? = when (option) {
    MEMORY_CARD_OPTION_GLOBAL, MEMORY_CARD_OPTION_CREATE -> null
    MEMORY_CARD_OPTION_NONE -> ""
    else -> cards.getOrNull(option - 1)?.name
}

/**
 * Card names created for every *other* game. They stay hidden from the local
 * selector so games do not accidentally pick a card owned by another game;
 * they remain fully visible in the memory card manager.
 */
fun foreignDedicatedCardNames(
    profiles: List<PerGameSettings>,
    currentGameKey: String
): Set<String> = profiles.asSequence()
    .filter { it.gameKey != currentGameKey }
    .flatMap { it.dedicatedMemoryCards.asSequence() }
    .map { it.lowercase() }
    .toSet()

/**
 * A game may only create its own card while none of the cards it registered
 * still exists; otherwise the "create" option would duplicate the card.
 */
fun canCreateDedicatedCard(
    dedicatedMemoryCards: List<String>,
    cards: List<MemoryCardInfo>
): Boolean = dedicatedMemoryCards.none { dedicated ->
    cards.any { it.name.equals(dedicated, ignoreCase = true) }
}

/** Base file name for a card created for a game: serial first, title second. */
fun perGameCardBaseName(gameSerial: String?, gameTitle: String): String =
    gameSerial?.takeIf { it.isNotBlank() } ?: gameTitle

/**
 * Legacy per-game core option values map onto the new override: "no card"
 * keeps the slot empty, everything else falls back to the global assignment
 * (the old files stay visible in the manager and can be picked manually).
 */
fun legacyMemoryCardSlotOverride(legacyCardType: String?): String? = when (legacyCardType) {
    "None", "NonPersistent" -> ""
    else -> null
}
