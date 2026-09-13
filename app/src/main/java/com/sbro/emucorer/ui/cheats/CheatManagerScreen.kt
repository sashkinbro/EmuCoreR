package com.sbro.emucorer.ui.cheats

import android.provider.OpenableColumns
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.automirrored.rounded.ArrowForward
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Folder
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sbro.emucorer.R
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.data.CheatBlock
import com.sbro.emucorer.data.CheatGameConfig
import com.sbro.emucorer.data.CheatRepository
import com.sbro.emucorer.data.ContentLibraryRepository
import com.sbro.emucorer.data.GameItem
import com.sbro.emucorer.data.GamePatchRepository
import com.sbro.emucorer.core.NativeApp
import com.sbro.emucorer.data.SelectedGameIdentity
import com.sbro.emucorer.ui.common.AppAlertDialog
import com.sbro.emucorer.ui.common.LibraryGamePicker
import com.sbro.emucorer.ui.common.ScreenTopBar
import com.sbro.emucorer.ui.common.appScreenTopPadding
import com.sbro.emucorer.ui.common.navigationBarsHorizontalPaddingValues
import com.sbro.emucorer.ui.theme.ScreenHorizontalPadding
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import com.sbro.emucorer.ui.theme.neon.neonShape
import com.sbro.emucorer.ui.theme.neon.neonButtonShape

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun CheatManagerScreen(onBackClick: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val preferences = remember(context) { AppPreferences(context) }
    val cheatRepository = remember(context) { CheatRepository(context) }
    val patchRepository = remember(context) { GamePatchRepository(context) }
    val libraryRepository = remember(context) { ContentLibraryRepository(context) }
    val cheatWriteMutex = remember { Mutex() }
    val cheatsEnabled by preferences.enableCheats.collectAsState(initial = false)
    val topInset = appScreenTopPadding()
    val bottomInset = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val horizontalSystemBarPadding = navigationBarsHorizontalPaddingValues()

    var games by remember { mutableStateOf<List<GameItem>>(emptyList()) }
    var selectedPath by remember { mutableStateOf<String?>(null) }
    var identity by remember { mutableStateOf<SelectedGameIdentity?>(null) }
    var config by remember { mutableStateOf<CheatGameConfig?>(null) }
    var resolvingIdentity by remember { mutableStateOf(false) }
    var pendingDelete by remember { mutableStateOf<CheatGameConfig?>(null) }
    var selectedCheatCategory by remember { mutableStateOf<CheatCategory?>(null) }
    var cheatSearchVisible by remember { mutableStateOf(false) }
    var cheatSearchQuery by remember { mutableStateOf("") }

    val importSuccess = stringResource(R.string.cheat_manager_import_success)
    val importFailure = stringResource(R.string.cheat_manager_import_failed)
    val deleteSuccess = stringResource(R.string.cheat_manager_delete_success)

    suspend fun loadConfig(selectedIdentity: SelectedGameIdentity?): CheatGameConfig? = withContext(Dispatchers.IO) {
        val serial = selectedIdentity?.serial.orEmpty()
        val crc = selectedIdentity?.crc
        val keys = buildList {
            if (serial.isNotBlank() && !crc.isNullOrBlank()) add("${serial}_$crc")
            if (!crc.isNullOrBlank()) add(crc)
            if (serial.isNotBlank()) add(serial)
        }
        cheatRepository.getGameConfig(keys, serial, crc)
    }

    LaunchedEffect(Unit) {
        games = withContext(Dispatchers.IO) { libraryRepository.loadGames() }
        selectedPath = games.firstOrNull()?.path
    }

    val selectedGame = games.firstOrNull { it.path == selectedPath }
    LaunchedEffect(selectedPath) {
        val game = selectedGame
        if (game == null) {
            identity = null
            config = null
            return@LaunchedEffect
        }
        identity = null
        resolvingIdentity = true
        val resolved = withContext(Dispatchers.IO) { libraryRepository.resolveIdentity(game) }
        if (selectedPath != game.path) return@LaunchedEffect
        identity = resolved
        config = loadConfig(resolved)
        resolvingIdentity = false
    }

    LaunchedEffect(config?.gameKey) {
        selectedCheatCategory = null
        cheatSearchVisible = false
        cheatSearchQuery = ""
    }

    val installedBlocks = config?.blocks.orEmpty()
    val categoryGroups = remember(installedBlocks) {
        CheatCategory.entries.mapNotNull { category ->
            installedBlocks.filter { block -> block.category() == category }
                .takeIf(List<CheatBlock>::isNotEmpty)
                ?.let { category to it }
        }
    }
    val visibleInstalledBlocks = remember(installedBlocks, categoryGroups, selectedCheatCategory, cheatSearchQuery) {
        when {
            cheatSearchQuery.isNotBlank() -> installedBlocks.filter { block ->
                block.title.contains(cheatSearchQuery.trim(), ignoreCase = true) ||
                    block.author?.contains(cheatSearchQuery.trim(), ignoreCase = true) == true
            }
            selectedCheatCategory != null -> categoryGroups
                .firstOrNull { it.first == selectedCheatCategory }
                ?.second
                .orEmpty()
            else -> emptyList()
        }
    }

    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val text = context.contentResolver.openInputStream(uri)?.use { it.readBytes().decodeToString() }
                        ?: return@runCatching 0
                    val fallbackName = runCatching {
                        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                            ?.use { cursor ->
                                if (cursor.moveToFirst()) cursor.getString(0) else null
                            }
                    }.getOrNull()?.substringBeforeLast('.')
                    val gameKey = identity?.let { resolved ->
                        if (!resolved.serial.isNullOrBlank() && !resolved.crc.isNullOrBlank()) {
                            "${resolved.serial}_${resolved.crc}"
                        } else resolved.crc ?: resolved.serial
                    } ?: fallbackName ?: "cheat_${System.currentTimeMillis()}"
                    cheatRepository.importCheatFile(gameKey, text, enableAllByDefault = false)
                }.getOrDefault(0)
            }
            if (result > 0) {
                preferences.setEnableCheats(true)
                config = loadConfig(identity)
            }
            Toast.makeText(context, if (result > 0) importSuccess else importFailure, Toast.LENGTH_LONG).show()
        }
    }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
        contentColor = MaterialTheme.colorScheme.onBackground
    ) {
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontalSystemBarPadding),
            contentPadding = PaddingValues(
                start = ScreenHorizontalPadding,
                end = ScreenHorizontalPadding,
                bottom = 24.dp + bottomInset
            ),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            item {
                ScreenTopBar(
                    title = stringResource(R.string.cheat_manager_title),
                    onBackClick = onBackClick,
                    modifier = Modifier.padding(top = topInset)
                )
            }
            item {
                Surface(
                    modifier = Modifier.fillMaxWidth(),
                    shape = neonShape(22.dp),
                    color = MaterialTheme.colorScheme.surface,
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
                ) {
                    Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    stringResource(R.string.cheat_manager_enable),
                                    style = MaterialTheme.typography.titleMedium,
                                    fontWeight = FontWeight.SemiBold
                                )
                                Text(
                                    stringResource(R.string.cheat_manager_enable_desc),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            Switch(
                                checked = cheatsEnabled,
                                onCheckedChange = { enabled ->
                                    scope.launch {
                                        preferences.setEnableCheats(enabled)
                                        val currentConfig = config
                                        if (currentConfig != null) {
                                            val patchBlocks = patchRepository.buildPatchBlocks(
                                                serial = currentConfig.serial,
                                                crc = currentConfig.crc,
                                                widescreen = preferences.enableWidescreenPatches.first(),
                                                noInterlacing = preferences.enableNoInterlacingPatches.first()
                                            )
                                            cheatRepository.syncActiveCheats(
                                                currentConfig.gameKey,
                                                currentConfig.serial,
                                                currentConfig.crc,
                                                includeCheats = enabled,
                                                patchBlocks = patchBlocks
                                            )
                                            val coreFile = cheatRepository.activeCoreCheatFile(
                                                currentConfig.gameKey,
                                                currentConfig.serial,
                                                currentConfig.crc
                                            )
                                            if (coreFile != null) {
                                                NativeApp.loadCheats(coreFile.absolutePath)
                                            } else {
                                                NativeApp.clearCheats()
                                            }
                                        } else {
                                            NativeApp.clearCheats()
                                        }
                                    }
                                }
                            )
                        }
                        OutlinedButton(
                            shape = neonButtonShape(),onClick = { importLauncher.launch(arrayOf("*/*")) }) {
                            Icon(Icons.Rounded.FolderOpen, contentDescription = null)
                            Spacer(Modifier.width(8.dp))
                            Text(stringResource(R.string.cheat_manager_import_pnach))
                        }
                    }
                }
            }
            item {
                LibraryGamePicker(
                    games = games,
                    selectedPath = selectedPath,
                    onSelected = { game ->
                        if (game.path != selectedPath) {
                            identity = null
                            config = null
                            resolvingIdentity = true
                            selectedPath = game.path
                        }
                    },
                    horizontalContentPadding = ScreenHorizontalPadding,
                    fullBleedPadding = ScreenHorizontalPadding
                )
            }
            if (selectedGame != null) {
                item {
                    Surface(
                        modifier = Modifier.fillMaxWidth(),
                        shape = neonShape(18.dp),
                        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f),
                        contentColor = MaterialTheme.colorScheme.onSurface
                    ) {
                        val serial = identity?.serial ?: selectedGame.serial
                        val crc = identity?.crc
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(14.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(10.dp)
                        ) {
                            if (resolvingIdentity) {
                                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                            }
                            Text(
                                text = when {
                                    resolvingIdentity -> serial ?: stringResource(R.string.content_serial_unknown)
                                    crc != null -> stringResource(
                                        R.string.cheat_manager_detected_identity,
                                        serial.orEmpty(),
                                        crc
                                    )
                                    else -> stringResource(
                                        R.string.cheat_manager_crc_unavailable,
                                        serial.orEmpty()
                                    )
                                },
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurface
                            )
                        }
                    }
                }
            }
            config?.let { current ->
                item(key = "installed-cheats-header") {
                    Row(
                        modifier = Modifier
                            .animateItem(
                                fadeInSpec = tween(220),
                                placementSpec = tween(220),
                                fadeOutSpec = tween(140)
                            )
                            .fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = stringResource(R.string.cheat_manager_installed),
                            modifier = Modifier.weight(1f),
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold
                        )
                        IconButton(
                            onClick = {
                                cheatSearchVisible = !cheatSearchVisible
                                selectedCheatCategory = null
                                if (!cheatSearchVisible) cheatSearchQuery = ""
                            }
                        ) {
                            Icon(
                                imageVector = if (cheatSearchVisible) Icons.Rounded.Close else Icons.Rounded.Search,
                                contentDescription = stringResource(R.string.cheat_manager_search)
                            )
                        }
                    }
                }
                if (cheatSearchVisible) {
                    item(key = "cheat-search-field") {
                        OutlinedTextField(
                            value = cheatSearchQuery,
                            onValueChange = { cheatSearchQuery = it },
                            modifier = Modifier
                                .animateItem(
                                    fadeInSpec = tween(220),
                                    placementSpec = tween(220),
                                    fadeOutSpec = tween(140)
                                )
                                .fillMaxWidth(),
                            placeholder = { Text(stringResource(R.string.cheat_manager_search)) },
                            leadingIcon = { Icon(Icons.Rounded.Search, contentDescription = null) },
                            trailingIcon = if (cheatSearchQuery.isNotEmpty()) {
                                {
                                    IconButton(onClick = { cheatSearchQuery = "" }) {
                                        Icon(
                                            Icons.Rounded.Close,
                                            contentDescription = stringResource(R.string.home_search_clear)
                                        )
                                    }
                                }
                            } else null,
                            singleLine = true,
                            shape = neonShape(18.dp)
                        )
                    }
                }
                if (!cheatSearchVisible) {
                    item(key = "cheat-folder-header") {
                        CheatCategoryBrowserHeader(
                            selectedCategory = selectedCheatCategory,
                            count = if (selectedCheatCategory == null) {
                                categoryGroups.sumOf { it.second.size }
                            } else {
                                visibleInstalledBlocks.size
                            },
                            onBack = { selectedCheatCategory = null },
                            modifier = Modifier.animateItem(
                                fadeInSpec = tween(220),
                                placementSpec = tween(220),
                                fadeOutSpec = tween(140)
                            )
                        )
                    }
                } else {
                    item(key = "cheat-search-header") {
                        Row(
                            modifier = Modifier
                                .animateItem(
                                    fadeInSpec = tween(220),
                                    placementSpec = tween(220),
                                    fadeOutSpec = tween(140)
                                )
                                .fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = stringResource(R.string.cheat_manager_search),
                                modifier = Modifier.weight(1f),
                                style = MaterialTheme.typography.titleMedium,
                                fontWeight = FontWeight.SemiBold
                            )
                            Text(
                                text = stringResource(
                                    R.string.cheat_manager_category_count,
                                    visibleInstalledBlocks.size
                                ),
                                style = MaterialTheme.typography.labelLarge,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
                if (!cheatSearchVisible && selectedCheatCategory == null) {
                    items(categoryGroups, key = { item -> "cheat-category-${item.first.name}" }) { (category, blocks) ->
                        CheatCategoryCard(
                            category = category,
                            count = blocks.size,
                            onClick = { selectedCheatCategory = category },
                            modifier = Modifier.animateItem(
                                fadeInSpec = tween(220),
                                placementSpec = tween(220),
                                fadeOutSpec = tween(140)
                            )
                        )
                    }
                } else {
                    if (visibleInstalledBlocks.isEmpty()) {
                        item(key = "cheat-search-empty") {
                            CheatSearchEmptyState(
                                modifier = Modifier.animateItem(
                                    fadeInSpec = tween(220),
                                    placementSpec = tween(220),
                                    fadeOutSpec = tween(140)
                                )
                            )
                        }
                    } else {
                        items(visibleInstalledBlocks, key = { block -> "cheat-block-${block.id}" }) { block ->
                            CheatToggleCard(
                                block = block,
                                onEnabledChange = { enabled ->
                                    val latest = config ?: current
                                    val enabledIds = latest.blocks
                                        .filter { it.enabled }
                                        .mapTo(mutableSetOf()) { it.id }
                                        .apply { if (enabled) add(block.id) else remove(block.id) }
                                    val updatedBlocks = latest.blocks.map { item ->
                                        if (item.id == block.id) item.copy(enabled = enabled) else item
                                    }
                                    config = latest.copy(blocks = updatedBlocks)
                                    scope.launch(Dispatchers.IO) {
                                        cheatWriteMutex.withLock {
                                            cheatRepository.setEnabledBlocks(latest.gameKey, enabledIds)
                                            cheatRepository.syncActiveCheats(
                                                latest.gameKey,
                                                latest.serial,
                                                latest.crc
                                            )
                                            val coreFile = cheatRepository.activeCoreCheatFile(
                                                latest.gameKey,
                                                latest.serial,
                                                latest.crc
                                            )
                                            if (coreFile != null) {
                                                NativeApp.loadCheats(coreFile.absolutePath)
                                            } else {
                                                NativeApp.clearCheats()
                                            }
                                        }
                                    }
                                },
                                modifier = Modifier.animateItem(
                                    fadeInSpec = tween(220),
                                    placementSpec = tween(220),
                                    fadeOutSpec = tween(140)
                                )
                            )
                        }
                    }
                }
                item(key = "cheat-delete") {
                    OutlinedButton(
                        shape = neonButtonShape(),
                        onClick = { pendingDelete = current },
                        modifier = Modifier.animateItem(
                            fadeInSpec = tween(220),
                            placementSpec = tween(220),
                            fadeOutSpec = tween(140)
                        )
                    ) {
                        Icon(Icons.Rounded.DeleteOutline, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.cheat_manager_delete))
                    }
                }
            }
        }
    }

    pendingDelete?.let { current ->
        AppAlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text(stringResource(R.string.cheat_manager_delete_title)) },
            text = { Text(stringResource(R.string.cheat_manager_delete_confirm)) },
            confirmButton = {
                TextButton(onClick = {
                    cheatRepository.deleteImportedCheats(current.gameKey, current.serial, current.crc)
                    NativeApp.clearCheats()
                    config = null
                    pendingDelete = null
                    Toast.makeText(context, deleteSuccess, Toast.LENGTH_SHORT).show()
                }) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }
}

@Composable
private fun CheatCategoryBrowserHeader(
    selectedCategory: CheatCategory?,
    count: Int,
    onBack: () -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (selectedCategory == null) {
            Icon(
                imageVector = Icons.Rounded.Folder,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary
            )
            Spacer(Modifier.width(9.dp))
            Text(
                text = stringResource(R.string.cheat_manager_categories),
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold
            )
        } else {
            TextButton(onClick = onBack, modifier = Modifier.weight(1f)) {
                Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = null)
                Spacer(Modifier.width(7.dp))
                Icon(Icons.Rounded.FolderOpen, contentDescription = null)
                Spacer(Modifier.width(7.dp))
                Text(
                    text = stringResource(selectedCategory.titleRes),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        Text(
            text = stringResource(R.string.cheat_manager_category_count, count),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun CheatCategoryCard(
    category: CheatCategory,
    count: Int,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    Surface(
        onClick = onClick,
        modifier = modifier.fillMaxWidth(),
        shape = neonShape(18.dp),
        color = MaterialTheme.colorScheme.surface,
        contentColor = MaterialTheme.colorScheme.onSurface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.72f))
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 15.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Surface(
                shape = neonShape(13.dp),
                color = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer
            ) {
                Icon(
                    imageVector = Icons.Rounded.Folder,
                    contentDescription = null,
                    modifier = Modifier.padding(10.dp).size(21.dp)
                )
            }
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(category.titleRes),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold
                )
                Text(
                    text = stringResource(R.string.cheat_manager_category_count, count),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Icon(
                imageVector = Icons.AutoMirrored.Rounded.ArrowForward,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary
            )
        }
    }
}

@Composable
private fun CheatToggleCard(
    block: CheatBlock,
    onEnabledChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = neonShape(16.dp),
        color = MaterialTheme.colorScheme.surface,
        contentColor = MaterialTheme.colorScheme.onSurface
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = block.title,
                    style = MaterialTheme.typography.bodyLarge
                )
                block.author?.takeIf { it.isNotBlank() }?.let { author ->
                    Text(
                        text = author,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
            Switch(
                checked = block.enabled,
                onCheckedChange = onEnabledChange
            )
        }
    }
}

@Composable
private fun CheatSearchEmptyState(modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = neonShape(18.dp),
        color = MaterialTheme.colorScheme.surface,
        contentColor = MaterialTheme.colorScheme.onSurfaceVariant
    ) {
        Text(
            text = stringResource(R.string.cheat_manager_search_empty),
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 18.dp),
            style = MaterialTheme.typography.bodyLarge
        )
    }
}

private enum class CheatCategory(val titleRes: Int) {
    PLAYER(R.string.cheat_manager_category_player),
    ITEMS(R.string.cheat_manager_category_items),
    WORLD(R.string.cheat_manager_category_world),
    PROGRESS(R.string.cheat_manager_category_progress),
    VEHICLES(R.string.cheat_manager_category_vehicles),
    STATS(R.string.cheat_manager_category_stats),
    HOTKEYS(R.string.cheat_manager_category_hotkeys),
    OTHER(R.string.cheat_manager_category_other)
}

private fun CheatBlock.category(): CheatCategory {
    val value = title.lowercase(Locale.US)
    return when {
        value.containsAny(" press ", "press ", "hold ", "button", "{l1}", "{l2}", "{r1}", "{r2}", "{select}") ->
            CheatCategory.HOTKEYS
        value.containsAny("health", "money", "pocket change", "stamina", "energy", "trouble", "wanted", "player", "character") ->
            CheatCategory.PLAYER
        value.containsAny("weapon", "ammo", "inventory", "item", "fire cracker", "spud", "slingshot", "projectile", "outfit", "clothing") ->
            CheatCategory.ITEMS
        value.containsAny("time", "hour", "clock", "weather", "day", "night", "season") ->
            CheatCategory.WORLD
        value.containsAny("mission", "chapter", "unlock", "class", "grade", "complete", "progress", "troph", "collectible") ->
            CheatCategory.PROGRESS
        value.containsAny("vehicle", "bike", "bicycle", "car", "kart", "race", "skateboard", "lawnmower") ->
            CheatCategory.VEHICLES
        value.startsWith("max ") || value.startsWith("no ") ||
            value.containsAny("stat", "record", "times ", "distance", "earned", "spent", "attempted", "hits", "killed", "thrown", "purchased") ->
            CheatCategory.STATS
        else -> CheatCategory.OTHER
    }
}

private fun String.containsAny(vararg needles: String): Boolean = needles.any(::contains)
