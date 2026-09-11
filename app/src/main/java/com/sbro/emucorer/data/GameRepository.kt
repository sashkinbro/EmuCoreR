package com.sbro.emucorer.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import com.sbro.emucorer.core.BiosValidator
import com.sbro.emucorer.core.DocumentPathResolver
import com.sbro.emucorer.core.EmulatorBridge
import com.sbro.emucorer.core.SetupValidator
import com.sbro.emucorer.data.ps1.Ps1TitleIndexRepository
import java.io.File
import java.io.Reader

data class GameItem(
    val title: String,
    val path: String,
    val fileName: String,
    val fileSize: Long,
    val lastModified: Long,
    val coverArtPath: String? = null,
    val serial: String? = null
)

class GameRepository {

    companion object {
        private const val TAG = "GameRepository"
        // Keep the library honest with what the bundled SwanStation core mounts.
        private val SUPPORTED_EXTENSIONS = setOf("bin", "cue", "iso", "img", "chd", "pbp", "ecm", "mds")
        private val COVER_EXTENSIONS = setOf("jpg", "jpeg", "png", "webp")
        private val COVER_DIRECTORY_NAMES = setOf("covers", "cover", "art", "artwork", "boxart", "box art")
        private const val MAX_DOCUMENT_SCAN_DEPTH = 32
        private const val MAX_DOCUMENT_SCAN_ENTRIES = 20_000
        private const val MAX_CUE_TEXT_CHARS = 256 * 1024
        private val CUE_FILE_DIRECTIVE = Regex(
            pattern = """^\s*FILE\s+(?:\"([^\"]+)\"|(\S+))\s+\S+""",
            option = RegexOption.IGNORE_CASE
        )

        /**
         * Returns normalized paths from CUE FILE directives. A CUE is the
         * launchable disc descriptor; its BIN files are tracks, not separate
         * library titles.
         */
        internal fun cueReferencedFiles(cueText: String): Set<String> =
            cueText.lineSequence()
                .mapNotNull { line ->
                    val match = CUE_FILE_DIRECTIVE.find(line) ?: return@mapNotNull null
                    (match.groups[1]?.value ?: match.groups[2]?.value)
                        ?.trim()
                        ?.replace('\\', '/')
                        ?.replace(Regex("/+"), "/")
                        ?.takeIf(String::isNotBlank)
                }
                .toSet()

        internal fun libraryIdentity(path: String): String {
            if (!path.startsWith("content://")) {
                return runCatching { File(path).canonicalPath }
                    .getOrDefault(File(path).absolutePath)
            }
            return runCatching {
                val uri = path.toUri()
                val documentId = DocumentsContract.getDocumentId(uri)
                "${uri.authority}:$documentId"
            }.getOrDefault(path)
        }

        internal fun parentDocumentId(documentId: String): String? {
            val separatorIndex = documentId.lastIndexOf('/')
            if (separatorIndex >= 0) return documentId.substring(0, separatorIndex)

            val volumeSeparatorIndex = documentId.indexOf(':')
            return if (volumeSeparatorIndex >= 0 && volumeSeparatorIndex < documentId.lastIndex) {
                documentId.substring(0, volumeSeparatorIndex + 1)
            } else {
                null
            }
        }

        internal fun relativeDocumentSegments(rootDocumentId: String, targetDocumentId: String): List<String>? {
            if (targetDocumentId == rootDocumentId) return emptyList()
            val prefix = if (rootDocumentId.endsWith(':')) rootDocumentId else "$rootDocumentId/"
            if (!targetDocumentId.startsWith(prefix)) return null
            return targetDocumentId.removePrefix(prefix).split('/').filter(String::isNotBlank)
        }
    }

    fun scanDirectory(path: String, context: Context): List<GameItem> {
        return scanDirectory(path, context, emptyMap())
    }

    fun scanDirectory(
        path: String,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean = { false },
        preferEnglishTitles: Boolean = false
    ): List<GameItem> {
        val dir = File(path)
        if (!dir.exists() || !dir.isDirectory) return emptyList()
        return scanLocalDirectory(
            dir,
            context,
            cachedGamesByPath,
            shouldAbort,
            preferEnglishTitles
        ).sortedBy { it.title.lowercase() }
    }

    fun scanDirectoryFromUri(uri: Uri, context: Context): List<GameItem> {
        return scanDirectoryFromUri(uri, context, emptyMap())
    }

    fun scanDirectoryFromUri(
        uri: Uri,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean = { false },
        preferEnglishTitles: Boolean = false
    ): List<GameItem> {
        if (!DocumentsContract.isTreeUri(uri)) {
            return emptyList()
        }
        return runCatching {
            val docFile = DocumentFile.fromTreeUri(context, uri) ?: return@runCatching emptyList()
            scanDocumentFile(
                docFile,
                context,
                cachedGamesByPath,
                shouldAbort,
                preferEnglishTitles = preferEnglishTitles,
                depth = 0,
                budget = DocumentScanBudget()
            )
                .sortedBy { it.title.lowercase() }
        }.onFailure { error ->
            Log.w(TAG, "Unable to scan document tree: $uri", error)
        }.getOrDefault(emptyList())
    }

    fun findCoverForGame(
        path: String,
        context: Context,
        serial: String? = null,
        title: String? = null
    ): String? {
        return if (path.startsWith("content://")) {
            runCatching { findDocumentCover(path, context, serial, title) }
                .onFailure { error -> Log.w(TAG, "Unable to find cover for document URI: $path", error) }
                .getOrNull()
        } else {
            findLocalCover(path, context, serial, title)
        }
    }

    fun downloadCoverForGame(game: GameItem, context: Context): String? {
        if (!game.coverArtPath.isNullOrBlank()) {
            val coverFile = File(game.coverArtPath)
            if (coverFile.exists()) {
                return game.coverArtPath
            }
        }

        return game.serial?.takeIf { it.isNotBlank() }?.let { serial ->
            CoverArtRepository(context).downloadCover(serial)
        } ?: Ps1TitleIndexRepository(context)
            .serialForTitle(game.title)
            ?.let { serial -> CoverArtRepository(context).downloadCover(serial) }
    }

    private fun scanLocalDirectory(
        dir: File,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean,
        preferEnglishTitles: Boolean = false,
        inheritedCueTracks: Set<String> = emptySet()
    ): List<GameItem> {
        val items = mutableListOf<GameItem>()
        val children = dir.listFiles().orEmpty()
        val cueIndex = buildLocalCueIndex(children)
        val cueTrackPaths = inheritedCueTracks + cueIndex.trackIdentities
        val coverCandidates = buildLocalCoverCandidates(children)
        val coverRepository = CoverArtRepository(context)
        val customCoverRepository = CustomGameCoverRepository(context)
        val titleIndex = Ps1TitleIndexRepository(context)

        children.forEach { file ->
            if (shouldAbort()) return items
            when {
                file.isDirectory && normalizeBaseName(file.name) !in COVER_DIRECTORY_NAMES -> {
                    items.addAll(
                        scanLocalDirectory(
                            file,
                            context,
                            cachedGamesByPath,
                            shouldAbort,
                            preferEnglishTitles,
                            cueTrackPaths
                        )
                    )
                }

                file.isFile && file.extension.lowercase() in SUPPORTED_EXTENSIONS &&
                    libraryIdentity(file.absolutePath) !in cueTrackPaths -> {
                    val cachedGame = cachedGamesByPath[file.absolutePath]
                    val canReuseCachedMetadata = cachedGame != null &&
                        cachedGame.fileSize == file.length() &&
                        cachedGame.lastModified == file.lastModified() &&
                        cachedGame.fileName == file.name &&
                        !cachedGame.serial.isNullOrBlank()
                    val metadata = if (canReuseCachedMetadata) {
                        com.sbro.emucorer.core.GameMetadata(cachedGame.title, cachedGame.serial)
                    } else {
                        val sourcePath = cueIndex.metadataSourceByCue[
                            libraryIdentity(file.absolutePath)
                        ] ?: file.absolutePath
                        val sourceMetadata = EmulatorBridge.getGameMetadata(sourcePath)
                        sourceMetadata.copy(
                            title = if (sourcePath == file.absolutePath) {
                                sourceMetadata.title
                            } else {
                                EmulatorBridge.cleanGameDisplayTitle(null, file.name)
                            }
                        )
                    }
                    if (BiosValidator.isLikelyBiosLibraryEntry(file.name, metadata.title, metadata.serial, file.length())) {
                        return@forEach
                    }

                    val cleanTitle = EmulatorBridge.cleanGameDisplayTitle(metadata.title, file.name)
                    val serial = metadata.serial
                        ?: titleIndex.serialForTitle(cleanTitle, regionHintForName(file.name))
                    val title = if (preferEnglishTitles) {
                        titleIndex.titleForSerial(serial) ?: cleanTitle
                    } else {
                        cleanTitle
                    }
                    items += GameItem(
                        title = title,
                        path = file.absolutePath,
                        fileName = file.name,
                        fileSize = file.length(),
                        lastModified = file.lastModified(),
                        coverArtPath = customCoverRepository.findCustomCoverPath(file.absolutePath)
                            ?: coverRepository.findCachedCoverPath(serial)
                            ?: cachedGame?.coverArtPath?.takeIf { File(it).exists() }
                            ?: coverCandidates[normalizeBaseName(file.nameWithoutExtension)]?.absolutePath
                            ?: coverCandidates[normalizeBaseName(cleanGameName(title))]?.absolutePath,
                        serial = serial
                    )
                }
            }
        }

        return items
    }

    private fun scanDocumentFile(
        docFile: DocumentFile,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean,
        preferEnglishTitles: Boolean = false,
        depth: Int,
        budget: DocumentScanBudget,
        inheritedCueTracks: Set<String> = emptySet()
    ): List<GameItem> {
        if (depth > MAX_DOCUMENT_SCAN_DEPTH || !budget.tryEnterDirectory(docFile.uri.toString())) {
            return emptyList()
        }
        val items = mutableListOf<GameItem>()
        val children = runCatching { docFile.listFiles() }.getOrDefault(emptyArray())
        val cueIndex = buildDocumentCueIndex(docFile, children, context)
        val cueTrackIdentities = inheritedCueTracks + cueIndex.trackIdentities
        val coverCandidates = buildDocumentCoverCandidates(children)
        val coverRepository = CoverArtRepository(context)
        val customCoverRepository = CustomGameCoverRepository(context)
        val titleIndex = Ps1TitleIndexRepository(context)

        for (file in children) {
            if (shouldAbort() || !budget.tryVisitEntry()) return items
            val mimeType = runCatching { file.type }.getOrNull()
            val name = documentDisplayName(context, file)
            when (SetupValidator.classifyDocumentEntry(mimeType, name)) {
                SetupValidator.DocumentEntryKind.DIRECTORY,
                SetupValidator.DocumentEntryKind.UNKNOWN -> {
                    if (normalizeBaseName(name) !in COVER_DIRECTORY_NAMES) {
                        items.addAll(
                            scanDocumentFile(
                                file,
                                context,
                                cachedGamesByPath,
                                shouldAbort,
                                preferEnglishTitles,
                                depth + 1,
                                budget,
                                inheritedCueTracks = cueTrackIdentities
                            )
                        )
                    }
                }
                SetupValidator.DocumentEntryKind.GAME_FILE -> {
                    val uriPath = file.uri.toString()
                    if (libraryIdentity(uriPath) in cueTrackIdentities) continue
                    val fileSize = runCatching { file.length() }.getOrDefault(0L)
                    val lastModified = runCatching { file.lastModified() }.getOrDefault(0L)

                    val cachedGame = cachedGamesByPath[uriPath]
                    val canReuseCachedMetadata = cachedGame != null &&
                        cachedGame.fileSize == fileSize &&
                        cachedGame.lastModified == lastModified &&
                        cachedGame.fileName == name &&
                        !cachedGame.serial.isNullOrBlank()
                    val metadata = if (canReuseCachedMetadata) {
                        com.sbro.emucorer.core.GameMetadata(cachedGame.title, cachedGame.serial)
                    } else {
                        val sourcePath = cueIndex.metadataSourceByCue[
                            libraryIdentity(uriPath)
                        ] ?: uriPath
                        val sourceMetadata = EmulatorBridge.getGameMetadata(sourcePath)
                        sourceMetadata.copy(
                            title = if (sourcePath == uriPath) {
                                sourceMetadata.title
                            } else {
                                EmulatorBridge.cleanGameDisplayTitle(null, name)
                            }
                        )
                    }

                    if (BiosValidator.isLikelyBiosLibraryEntry(name, metadata.title, metadata.serial, fileSize)) {
                        continue
                    }

                    val cleanTitle = cleanScannedTitle(metadata.title, name)
                    val serial = metadata.serial
                        ?: titleIndex.serialForTitle(cleanTitle, regionHintForName(name))
                    val title = if (preferEnglishTitles) {
                        titleIndex.titleForSerial(serial) ?: cleanTitle
                    } else {
                        cleanTitle
                    }
                    items += GameItem(
                        title = title,
                        path = uriPath,
                        fileName = name,
                        fileSize = fileSize,
                        lastModified = lastModified,
                        coverArtPath = customCoverRepository.findCustomCoverPath(uriPath)
                            ?: coverRepository.findCachedCoverUri(serial)
                            ?: cachedGame?.coverArtPath
                            ?: coverCandidates[normalizeBaseName(name.substringBeforeLast('.'))]?.uri?.toString()
                            ?: coverCandidates[normalizeBaseName(cleanGameName(title))]?.uri?.toString(),
                        serial = serial
                    )
                }
                SetupValidator.DocumentEntryKind.OTHER -> Unit
            }
        }

        return items
    }

    private data class CueIndex(
        val trackIdentities: Set<String>,
        val metadataSourceByCue: Map<String, String>
    )

    private fun buildLocalCueIndex(children: Array<out File>): CueIndex {
        val trackIdentities = LinkedHashSet<String>()
        val metadataSourceByCue = HashMap<String, String>()
        children.asSequence()
            .filter { it.isFile && it.extension.equals("cue", ignoreCase = true) }
            .forEach { cue ->
                val text = runCatching {
                    cue.inputStream().bufferedReader().use(::readLimitedCueText)
                }.getOrDefault("")
                val tracks = cueReferencedFiles(text).map { reference ->
                    File(cue.parentFile, reference)
                }
                tracks.forEach { track ->
                    trackIdentities += libraryIdentity(track.absolutePath)
                }
                tracks.firstOrNull { it.isFile }?.let { metadataTrack ->
                    metadataSourceByCue[libraryIdentity(cue.absolutePath)] =
                        metadataTrack.absolutePath
                }
            }
        return CueIndex(trackIdentities, metadataSourceByCue)
    }

    private fun buildDocumentCueIndex(
        directory: DocumentFile,
        children: Array<DocumentFile>,
        context: Context
    ): CueIndex {
        val trackIdentities = LinkedHashSet<String>()
        val metadataSourceByCue = HashMap<String, String>()
        children.asSequence()
            .filter { document ->
                documentDisplayName(context, document)
                    .substringAfterLast('.', "")
                    .equals("cue", ignoreCase = true)
            }
            .forEach { cue ->
                val text = runCatching {
                    context.contentResolver.openInputStream(cue.uri)
                        ?.bufferedReader()
                        ?.use(::readLimitedCueText)
                        .orEmpty()
                }.getOrDefault("")
                val tracks = cueReferencedFiles(text).mapNotNull { reference ->
                    resolveDocumentReference(directory, reference, context)
                }
                tracks.forEach { track ->
                    trackIdentities += libraryIdentity(track.uri.toString())
                }
                tracks.firstOrNull()?.let { metadataTrack ->
                    metadataSourceByCue[libraryIdentity(cue.uri.toString())] =
                        metadataTrack.uri.toString()
                }
            }
        return CueIndex(trackIdentities, metadataSourceByCue)
    }

    private fun resolveDocumentReference(
        directory: DocumentFile,
        reference: String,
        context: Context
    ): DocumentFile? {
        var current = directory
        val segments = reference.replace('\\', '/').split('/')
            .filter { it.isNotBlank() && it != "." }
        if (segments.isEmpty() || segments.any { it == ".." }) return null
        for ((index, segment) in segments.withIndex()) {
            val child = runCatching { current.listFiles() }
                .getOrDefault(emptyArray())
                .firstOrNull {
                    documentDisplayName(context, it).equals(segment, ignoreCase = true)
                } ?: return null
            if (index == segments.lastIndex) return child.takeIf { it.isFile }
            if (!child.isDirectory) return null
            current = child
        }
        return null
    }

    private fun readLimitedCueText(reader: Reader): String {
        val result = StringBuilder()
        val buffer = CharArray(4096)
        while (result.length < MAX_CUE_TEXT_CHARS) {
            val count = reader.read(
                buffer,
                0,
                minOf(buffer.size, MAX_CUE_TEXT_CHARS - result.length)
            )
            if (count <= 0) break
            result.append(buffer, 0, count)
        }
        return result.toString()
    }

    private fun findLocalCover(path: String, context: Context, serial: String?, title: String?): String? {
        val file = File(path)
        val parent = file.parentFile ?: return null
        val baseName = normalizeBaseName(file.nameWithoutExtension)
        val titleKey = normalizeBaseName(cleanGameName(title ?: EmulatorBridge.getGameTitle(path)))
        val coverCandidates = buildLocalCoverCandidates(parent.listFiles().orEmpty())
        return CustomGameCoverRepository(context).findCustomCoverPath(path)
            ?: CoverArtRepository(context).findCachedCoverPath(serial)
            ?: coverCandidates[baseName]?.absolutePath
            ?: coverCandidates[titleKey]?.absolutePath
    }

    private fun findDocumentCover(path: String, context: Context, serial: String?, title: String?): String? {
        CustomGameCoverRepository(context).findCustomCoverPath(path)?.let { return it }
        CoverArtRepository(context).findCachedCoverUri(serial)?.let { return it }

        val uri = path.toUri()
        val document = DocumentFile.fromSingleUri(context, uri) ?: return null
        val parent = findDocumentParent(context, uri) ?: return null
        val baseName = normalizeBaseName(document.name.orEmpty().substringBeforeLast('.'))
        val titleKey = normalizeBaseName(cleanGameName(title ?: document.name.orEmpty().substringBeforeLast('.')))
        val coverCandidates = buildDocumentCoverCandidates(parent.listFiles())
        return coverCandidates[baseName]?.uri?.toString()
            ?: coverCandidates[titleKey]?.uri?.toString()
    }

    private fun findDocumentParent(context: Context, documentUri: Uri): DocumentFile? {
        val documentId = DocumentsContract.getDocumentId(documentUri)
        val parentId = parentDocumentId(documentId) ?: return null
        val persistedTrees = context.contentResolver.persistedUriPermissions
            .asSequence()
            .filter { permission -> permission.isReadPermission && permission.uri.authority == documentUri.authority }
            .mapNotNull { permission ->
                val rootId = runCatching { DocumentsContract.getTreeDocumentId(permission.uri) }.getOrNull()
                    ?: return@mapNotNull null
                Triple(permission.uri, rootId, relativeDocumentSegments(rootId, parentId))
            }
            .filter { (_, _, segments) -> segments != null }
            .sortedByDescending { (_, rootId, _) -> rootId.length }
            .toList()

        for ((treeUri, _, nullableSegments) in persistedTrees) {
            var current = DocumentFile.fromTreeUri(context, treeUri) ?: continue
            var found = true
            for (segment in nullableSegments.orEmpty()) {
                current = current.findFile(segment) ?: run {
                    found = false
                    break
                }
            }
            if (found) return current
        }

        return null
    }

    private fun normalizeBaseName(value: String): String {
        return value.lowercase()
            .replace('_', ' ')
            .replace('-', ' ')
            .replace(Regex("""\[[^]]*]|\([^)]*\)"""), " ")
            .replace(Regex("""\b(disc|disk|cd|dvd)\s*[0-9]+\b"""), " ")
            .replace(Regex("""\b\(?(usa|eur|europe|japan|jpn|beta|demo|proto|prototype|rev\s*[a-z0-9]+)\)?\b"""), " ")
            .replace(Regex("\\s+"), " ")
            .trim()
    }

    private fun cleanGameName(value: String): String {
        return value.substringBeforeLast('.')
            .replace(Regex("""\s+"""), " ")
            .trim()
    }

    /** Region hint parsed from a dump filename, used to disambiguate serials. */
    private fun regionHintForName(name: String): Char? {
        val lower = name.lowercase()
        return when {
            "usa" in lower || "(u)" in lower || "ntsc-u" in lower || "us " in lower -> 'U'
            "europe" in lower || "(e)" in lower || "ntsc-e" in lower || "eur" in lower || "pal " in lower -> 'E'
            "japan" in lower || "(j)" in lower || "ntsc-j" in lower || "jpn" in lower -> 'J'
            else -> null
        }
    }

    private fun cleanScannedTitle(rawTitle: String, displayName: String): String {
        return EmulatorBridge.cleanGameDisplayTitle(rawTitle, displayName)
    }

    private fun buildLocalCoverCandidates(children: Array<out File>): Map<String, File> {
        val directFiles = children.filter { it.isFile && it.extension.lowercase() in COVER_EXTENSIONS }
        val nestedCoverFiles = children
            .filter { it.isDirectory && normalizeBaseName(it.name) in COVER_DIRECTORY_NAMES }
            .flatMap { it.listFiles().orEmpty().asIterable() }
            .filter { it.isFile && it.extension.lowercase() in COVER_EXTENSIONS }

        return (directFiles + nestedCoverFiles).associateBy { normalizeBaseName(cleanGameName(it.nameWithoutExtension)) }
    }

    private fun buildDocumentCoverCandidates(
        children: Array<DocumentFile>
    ): Map<String, DocumentFile> {
        val directFiles = children
            .filter {
                runCatching { it.type }.getOrNull() != DocumentsContract.Document.MIME_TYPE_DIR &&
                    documentDisplayName(null, it)
                        .substringAfterLast('.', "")
                        .lowercase() in COVER_EXTENSIONS
            }
        val nestedCoverFiles = children
            .filter {
                val type = runCatching { it.type }.getOrNull()
                (type == DocumentsContract.Document.MIME_TYPE_DIR || type == null) &&
                    normalizeBaseName(documentDisplayName(null, it)) in COVER_DIRECTORY_NAMES
            }
            .flatMap { runCatching { it.listFiles().asIterable() }.getOrDefault(emptyList()) }
            .filter {
                runCatching { it.type }.getOrNull() != DocumentsContract.Document.MIME_TYPE_DIR &&
                    documentDisplayName(null, it)
                        .substringAfterLast('.', "")
                        .lowercase() in COVER_EXTENSIONS
            }

        return (directFiles + nestedCoverFiles).associateBy {
            normalizeBaseName(cleanGameName(documentDisplayName(null, it).substringBeforeLast('.')))
        }
    }

    private fun documentDisplayName(context: Context?, document: DocumentFile): String {
        val rawName = runCatching { document.name }.getOrNull().orEmpty()
        if (rawName.isNotBlank()) return DocumentPathResolver.normalizeDisplayName(rawName)
        if (context == null) return ""
        return runCatching { DocumentPathResolver.getDisplayName(context, document.uri.toString()) }
            .getOrDefault("")
    }

    private class DocumentScanBudget {
        private var visitedEntries = 0
        private val visitedDirectories = HashSet<String>()

        fun tryEnterDirectory(uri: String): Boolean = visitedDirectories.add(uri)

        fun tryVisitEntry(): Boolean {
            if (visitedEntries >= MAX_DOCUMENT_SCAN_ENTRIES) return false
            visitedEntries++
            return true
        }
    }
}
