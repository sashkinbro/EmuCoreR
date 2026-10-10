package com.sbro.emucorer.data

import android.content.Context
import android.net.Uri
import com.sbro.emucorer.core.DocumentPathResolver
import com.sbro.emucorer.core.EmulatorBridge
import com.sbro.emucorer.core.EmulatorStorage
import com.sbro.emucorer.core.NativeApp
import java.io.File
import java.io.InputStream
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.flow.first

data class MemoryCardInfo(
    val name: String,
    val path: String,
    val modifiedTime: Long,
    val type: Int,
    val fileType: Int,
    val sizeBytes: Long,
    val formatted: Boolean,
    val isDefaultCard: Boolean
)

data class MemoryCardAssignments(
    val slot1: String?,
    val slot2: String?
)

/** What a single memory-card write did, so the UI can report it precisely. */
data class MemoryCardWriteResult(
    val success: Boolean,
    val cardName: String? = null,
    val requestedName: String? = null,
    val previousName: String? = null,
    val renamedToAvoidConflict: Boolean = false,
    val clearedSlots: List<Int> = emptyList()
)

/** Outcome of assigning a card to a slot, or clearing one. */
data class MemoryCardAssignResult(
    val success: Boolean,
    val cardName: String? = null,
    val slot: Int = 1,
    val clearedSlots: List<Int> = emptyList()
)

/** A card written by a restore/import batch. */
data class MemoryCardRestoredCard(
    val name: String,
    val overwritten: Boolean
)

/** Outcome of a restore/import: which cards were written and which were replaced. */
data class MemoryCardRestoreResult(
    val success: Boolean,
    val cards: List<MemoryCardRestoredCard> = emptyList()
) {
    val createdCount: Int get() = cards.count { !it.overwritten }
    val overwrittenCount: Int get() = cards.count { it.overwritten }
}

/** Owns raw 128 KiB PlayStation memory-card images. */
class MemoryCardRepository(
    private val context: Context,
    private val preferences: AppPreferences
) {

    suspend fun ensureDefaultCardsAssigned(): MemoryCardAssignments {
        syncNativeMemoryCardDirectory()
        val current = currentAssignments()
        if (preferences.memoryCardsInitialized.first()) return current

        // No initialized marker yet: either a fresh install or a pre-flag upgrade.
        // Upgrades keep whatever the user had configured, including empty slots.
        if (current.slot1 != null || current.slot2 != null) {
            preferences.markMemoryCardsInitialized()
            return current
        }
        if (!NativeApp.hasNativeCore) return current

        val existingNames = listCards().mapTo(mutableSetOf()) { it.name }
        if (DEFAULT_CARD_SLOT_1 !in existingNames && createPs1Card(DEFAULT_CARD_SLOT_1).success) {
            existingNames += DEFAULT_CARD_SLOT_1
        }
        if (DEFAULT_CARD_SLOT_2 !in existingNames && createPs1Card(DEFAULT_CARD_SLOT_2).success) {
            existingNames += DEFAULT_CARD_SLOT_2
        }
        if (existingNames.isEmpty()) return current

        val resolvedSlot1 = DEFAULT_CARD_SLOT_1.takeIf(existingNames::contains) ?: existingNames.first()
        val resolvedSlot2 = DEFAULT_CARD_SLOT_2.takeIf {
            it in existingNames && !it.equals(resolvedSlot1, ignoreCase = true)
        } ?: existingNames.firstOrNull { !it.equals(resolvedSlot1, ignoreCase = true) }

        assignSlots(resolvedSlot1, resolvedSlot2)
        preferences.markMemoryCardsInitialized()
        return MemoryCardAssignments(resolvedSlot1, resolvedSlot2)
    }

    fun listCards(): List<MemoryCardInfo> {
        syncNativeMemoryCardDirectory()
        return NativeApp.parseMemoryCardList(NativeApp.listMemoryCards())
            .asSequence()
            .filter { it.name.isSupportedMemoryCardName() && it.sizeBytes == PS1_MEMORY_CARD_SIZE_BYTES }
            .map {
                MemoryCardInfo(
                    name = it.name,
                    path = it.path,
                    modifiedTime = it.modifiedTime.normalizeEpochMillis(),
                    type = MEMORY_CARD_TYPE_FILE,
                    fileType = MEMORY_CARD_FILE_TYPE_PS1,
                    sizeBytes = it.sizeBytes,
                    formatted = true,
                    isDefaultCard = isDefaultMemoryCardName(it.name)
                )
            }
            .sortedBy { it.name.lowercase() }
            .toList()
    }

    fun createPs1Card(name: String): MemoryCardWriteResult {
        syncNativeMemoryCardDirectory()
        val requested = normalizeName(name)
        val target = buildUniqueCardName(name)
        val created = NativeApp.createMemoryCard(target, MEMORY_CARD_TYPE_FILE, MEMORY_CARD_FILE_TYPE_PS1)
        return MemoryCardWriteResult(
            success = created,
            cardName = target,
            requestedName = requested,
            renamedToAvoidConflict = !target.equals(requested, ignoreCase = true)
        )
    }

    /** Imports raw DuckStation, SwanStation, or EmuCoreR cards only when byte-compatible. */
    fun importCard(uri: Uri, displayName: String? = null): MemoryCardWriteResult {
        val resolvedName = displayName?.takeIf(String::isNotBlank)
            ?: DocumentPathResolver.getDisplayName(context, uri.toString())
        if (!resolvedName.isSupportedMemoryCardName()) return MemoryCardWriteResult(success = false)
        val requested = normalizeName(resolvedName)
        val target = File(memoryCardsDir(), buildUniqueCardName(resolvedName))
        val imported = context.contentResolver.openInputStream(uri)?.use { input ->
            importExactCard(input, target)
        } ?: false
        return MemoryCardWriteResult(
            success = imported,
            cardName = target.name,
            requestedName = requested,
            renamedToAvoidConflict = !target.name.equals(requested, ignoreCase = true)
        )
    }

    fun duplicateCard(card: MemoryCardInfo, newName: String): MemoryCardWriteResult {
        val source = File(card.path)
        if (!source.isValidPs1Card()) return MemoryCardWriteResult(success = false)
        val requested = normalizeName(newName)
        val target = File(memoryCardsDir(), buildUniqueCardName(newName))
        val copied = source.inputStream().use { importExactCard(it, target) }
        return MemoryCardWriteResult(
            success = copied,
            cardName = target.name,
            requestedName = requested,
            renamedToAvoidConflict = !target.name.equals(requested, ignoreCase = true)
        )
    }

    suspend fun renameCard(card: MemoryCardInfo, newName: String): MemoryCardWriteResult {
        if (card.isDefaultCard || isDefaultMemoryCardName(card.name)) {
            return MemoryCardWriteResult(success = false, cardName = card.name)
        }
        val source = File(card.path)
        if (!source.isValidPs1Card()) return MemoryCardWriteResult(success = false, cardName = card.name)
        val targetName = normalizeName(newName)
        if (targetName.equals(card.name, ignoreCase = true)) {
            return MemoryCardWriteResult(success = true, cardName = card.name)
        }
        val target = File(memoryCardsDir(), buildUniqueCardName(targetName, source.name))
        if (!source.renameTo(target)) {
            return MemoryCardWriteResult(success = false, cardName = card.name)
        }

        val assignments = currentAssignments()
        assignSlots(
            assignments.slot1.renameIfMatching(card.name, target.name),
            assignments.slot2.renameIfMatching(card.name, target.name)
        )
        return MemoryCardWriteResult(
            success = true,
            cardName = target.name,
            previousName = card.name
        )
    }

    suspend fun deleteCard(card: MemoryCardInfo): MemoryCardWriteResult {
        val name = card.name
        if (card.isDefaultCard || isDefaultMemoryCardName(name)) {
            return MemoryCardWriteResult(success = false, cardName = name)
        }
        val file = File(card.path)
        if (!file.isFile || !file.isSafelyInside(memoryCardsDir()) || !file.delete()) {
            return MemoryCardWriteResult(success = false, cardName = name)
        }
        val assignments = currentAssignments()
        val clearedSlots = buildList {
            if (assignments.slot1.equals(name, ignoreCase = true)) add(1)
            if (assignments.slot2.equals(name, ignoreCase = true)) add(2)
        }
        assignSlots(
            assignments.slot1.takeUnless { it.equals(name, ignoreCase = true) },
            assignments.slot2.takeUnless { it.equals(name, ignoreCase = true) }
        )
        return MemoryCardWriteResult(success = true, cardName = name, clearedSlots = clearedSlots)
    }

    fun exportCard(card: MemoryCardInfo, destination: Uri): Boolean {
        val source = File(card.path)
        if (!source.isValidPs1Card()) return false
        return runCatching {
            context.contentResolver.openOutputStream(destination)?.use { output ->
                source.inputStream().use { it.copyTo(output) }
            } != null
        }.getOrDefault(false)
    }

    /** Zips the given cards into [destination]; returns how many were written. */
    fun backupCards(cards: List<MemoryCardInfo>, destination: Uri): Int {
        val files = cards.map { File(it.path) }.filter(File::isValidPs1Card)
        if (files.isEmpty()) return 0
        return runCatching {
            context.contentResolver.openOutputStream(destination)?.use { output ->
                ZipOutputStream(output).use { zip ->
                    files.forEach { file ->
                        zip.putNextEntry(ZipEntry("memory-cards/${file.name}"))
                        file.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                    }
                }
            }
            files.size
        }.getOrDefault(0)
    }

    fun restoreCards(source: Uri): MemoryCardRestoreResult {
        val displayName = DocumentPathResolver.getDisplayName(context, source.toString())
        val mimeType = context.contentResolver.getType(source).orEmpty().lowercase(Locale.ROOT)
        return if (mimeType.contains("zip") || displayName.endsWith(".zip", ignoreCase = true)) {
            restoreCardsFromZip(source)
        } else {
            val imported = importCard(source, displayName)
            MemoryCardRestoreResult(
                success = imported.success,
                cards = if (imported.success && imported.cardName != null) {
                    listOf(MemoryCardRestoredCard(imported.cardName, overwritten = false))
                } else {
                    emptyList()
                }
            )
        }
    }

    suspend fun currentAssignments(): MemoryCardAssignments {
        val settings = preferences.exportJson()
        return MemoryCardAssignments(
            settings.optString("memoryCardSlot1").takeIf(String::isNotBlank),
            settings.optString("memoryCardSlot2").takeIf(String::isNotBlank)
        )
    }

    suspend fun assignCardToSlot(slot: Int, cardName: String?): MemoryCardAssignResult {
        val slotIndex = slot.coerceIn(1, 2)
        val current = currentAssignments()
        val clearedSlots = mutableListOf<Int>()
        val updated = when (slotIndex) {
            1 -> {
                if (current.slot2.equals(cardName, ignoreCase = true)) clearedSlots += 2
                MemoryCardAssignments(cardName, current.slot2.takeUnless { it.equals(cardName, ignoreCase = true) })
            }

            else -> {
                if (current.slot1.equals(cardName, ignoreCase = true)) clearedSlots += 1
                MemoryCardAssignments(current.slot1.takeUnless { it.equals(cardName, ignoreCase = true) }, cardName)
            }
        }
        assignSlots(updated.slot1, updated.slot2)
        return MemoryCardAssignResult(
            success = true,
            cardName = cardName,
            slot = slotIndex,
            clearedSlots = clearedSlots
        )
    }

    private suspend fun assignSlots(slot1: String?, slot2: String?) {
        preferences.setMemoryCardAssignments(slot1, slot2)
        EmulatorBridge.setMemoryCardAssignments(slot1, slot2)
    }

    private fun restoreCardsFromZip(source: Uri): MemoryCardRestoreResult {
        val restored = mutableListOf<MemoryCardRestoredCard>()
        val opened = runCatching {
            context.contentResolver.openInputStream(source)?.use { input ->
                ZipInputStream(input).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        val cardName = entry.safeMemoryCardName()
                        if (!entry.isDirectory && cardName != null) {
                            val target = File(memoryCardsDir(), normalizeName(cardName))
                            val overwritten = target.exists()
                            if (importExactCard(zip, target)) {
                                restored += MemoryCardRestoredCard(target.name, overwritten)
                            }
                        }
                        zip.closeEntry()
                    }
                }
            }
        }.getOrNull()
        return MemoryCardRestoreResult(success = opened != null && restored.isNotEmpty(), cards = restored)
    }

    private fun importExactCard(input: InputStream, target: File): Boolean {
        val directory = memoryCardsDir().apply { mkdirs() }
        val temporary = File.createTempFile(".card-import-", ".tmp", directory)
        val copied = runCatching {
            temporary.outputStream().use { output ->
                val buffer = ByteArray(8192)
                var total = 0L
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    total += read
                    if (total > PS1_MEMORY_CARD_SIZE_BYTES) return@runCatching false
                    output.write(buffer, 0, read)
                }
                total == PS1_MEMORY_CARD_SIZE_BYTES
            }
        }.getOrDefault(false)
        if (!copied) {
            temporary.delete()
            return false
        }
        return replaceValidatedCard(temporary, target)
    }

    private fun replaceValidatedCard(temporary: File, target: File): Boolean {
        if (!temporary.isValidPs1CardPayload() || !target.isSafelyInside(memoryCardsDir())) {
            temporary.delete()
            return false
        }
        val previous = if (target.exists()) {
            File.createTempFile(".card-backup-", ".tmp", memoryCardsDir()).also { it.delete() }
        } else {
            null
        }
        if (previous != null && !target.renameTo(previous)) {
            temporary.delete()
            return false
        }
        if (temporary.renameTo(target)) {
            previous?.delete()
            return true
        }
        previous?.renameTo(target)
        temporary.delete()
        return false
    }

    private fun buildUniqueCardName(value: String, currentName: String? = null): String {
        val normalized = normalizeName(value)
        val baseName = normalized.removeSuffix(PS1_MEMORY_CARD_EXTENSION)
        var candidate = normalized
        var index = 2
        while (true) {
            val existing = File(memoryCardsDir(), candidate)
            if (!existing.exists() || candidate.equals(currentName, ignoreCase = true)) return candidate
            candidate = "$baseName ($index)$PS1_MEMORY_CARD_EXTENSION"
            index++
        }
    }

    private fun normalizeName(value: String): String {
        val trimmed = value.trim()
        val baseName = if (trimmed.isSupportedMemoryCardName()) trimmed.substringBeforeLast('.') else trimmed
        val safeBase = baseName
            .replace(Regex("[^a-zA-Z0-9._ -]"), "_")
            .replace(Regex("\\s+"), " ")
            .trim().trim('.', ' ')
            .ifBlank { "Memory Card" }
        return "$safeBase$PS1_MEMORY_CARD_EXTENSION"
    }

    private fun syncNativeMemoryCardDirectory() {
        runCatching {
            NativeApp.beginSettingsBatch()
            NativeApp.setSetting("Folders", "MemoryCards", "string", memoryCardsDir().absolutePath)
            NativeApp.endSettingsBatch()
        }
    }

    private fun memoryCardsDir(): File =
        EmulatorStorage.memoryCardsDir(context, preferences.getEmulatorDataPathSync()).apply { mkdirs() }

    /** Player preference: report card operations back in the app UI. */
    suspend fun notificationsEnabled(): Boolean = preferences.memoryCardNotifications.first()
}

internal const val MEMORY_CARD_TYPE_FILE = 1
internal const val MEMORY_CARD_FILE_TYPE_PS1 = 1
internal const val PS1_MEMORY_CARD_SIZE_BYTES = 128L * 1024L
private const val PS1_MEMORY_CARD_EXTENSION = ".mcd"
private const val DEFAULT_CARD_SLOT_1 = "MemoryCard1.mcd"
private const val DEFAULT_CARD_SLOT_2 = "MemoryCard2.mcd"

private fun isDefaultMemoryCardName(name: String): Boolean =
    name.equals(DEFAULT_CARD_SLOT_1, true) || name.equals(DEFAULT_CARD_SLOT_2, true)

private fun String?.renameIfMatching(oldName: String, newName: String): String? =
    if (this.equals(oldName, true)) newName else this

private fun Long.normalizeEpochMillis(): Long =
    if (this in 1 until 1_000_000_000_000L) this * 1000L else this

private fun String.isSupportedMemoryCardName(): Boolean =
    endsWith(".mcd", true) || endsWith(".mcr", true) || endsWith(".bin", true)

private fun ZipEntry.safeMemoryCardName(): String? {
    val parts = name.replace('\\', '/').trim('/').split('/').filter(String::isNotBlank)
    if (parts.isEmpty() || parts.any { it == "." || it == ".." }) return null
    val candidate = parts.last()
    return candidate.takeIf(String::isSupportedMemoryCardName)
}

private fun File.isValidPs1Card(): Boolean =
    isFile && name.isSupportedMemoryCardName() && length() == PS1_MEMORY_CARD_SIZE_BYTES

private fun File.isValidPs1CardPayload(): Boolean = isFile && length() == PS1_MEMORY_CARD_SIZE_BYTES

private fun File.isSafelyInside(root: File): Boolean {
    val rootPath = root.canonicalFile.toPath()
    val filePath = canonicalFile.toPath()
    return filePath != rootPath && filePath.startsWith(rootPath)
}
