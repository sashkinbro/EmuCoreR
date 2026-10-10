package com.sbro.emucorer.ui.memorycards

import android.annotation.SuppressLint
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.CloudDownload
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.Edit
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Save
import com.sbro.emucorer.ui.common.AppAlertDialog as AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.sbro.emucorer.R
import com.sbro.emucorer.core.EmulatorBridge
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.data.MemoryCardAssignments
import com.sbro.emucorer.data.MemoryCardInfo
import com.sbro.emucorer.data.MemoryCardRepository
import com.sbro.emucorer.ui.common.ScreenTopBar
import com.sbro.emucorer.ui.common.appScreenTopPadding
import com.sbro.emucorer.ui.common.navigationBarsHorizontalPaddingValues
import com.sbro.emucorer.ui.theme.ScreenHorizontalPadding
import com.sbro.emucorer.ui.theme.neon.LocalNeonTheme
import com.sbro.emucorer.ui.theme.neon.NeonSystemBanner
import com.sbro.emucorer.ui.theme.neon.neonButtonShape
import com.sbro.emucorer.ui.theme.neon.neonChipShape
import com.sbro.emucorer.ui.theme.neon.neonShape
import java.text.DateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun MemoryCardManagerScreen(
    onBackClick: () -> Unit
) {
    val context = LocalContext.current
    val repository = remember(context) { MemoryCardRepository(context, AppPreferences(context)) }
    val scope = rememberCoroutineScope()
    val topInset = appScreenTopPadding()
    val bottomInset = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val horizontalSystemBarPadding = navigationBarsHorizontalPaddingValues()

    val createSuccessTemplate = stringResource(R.string.memory_card_create_success)
    val createRenamedTemplate = stringResource(R.string.memory_card_create_renamed)
    val createFailureMessage = stringResource(R.string.memory_card_create_failed)
    val backupSuccessTemplate = stringResource(R.string.memory_card_backup_success)
    val backupFailureMessage = stringResource(R.string.memory_card_backup_failed)
    val restoreSuccessTemplate = stringResource(R.string.memory_card_restore_success)
    val restoreFailureMessage = stringResource(R.string.memory_card_restore_failed)
    val importedTemplate = stringResource(R.string.memory_card_imported)
    val importOverwrittenTemplate = stringResource(R.string.memory_card_import_overwritten)
    val exportSuccessTemplate = stringResource(R.string.memory_card_export_success)
    val exportFailureMessage = stringResource(R.string.memory_card_export_failed)
    val duplicateSuccessTemplate = stringResource(R.string.memory_card_duplicate_success)
    val duplicateFailureMessage = stringResource(R.string.memory_card_duplicate_failed)
    val renameSuccessTemplate = stringResource(R.string.memory_card_rename_success)
    val renameFailureMessage = stringResource(R.string.memory_card_rename_failed)
    val deleteSuccessTemplate = stringResource(R.string.memory_card_delete_success)
    val deleteFailureMessage = stringResource(R.string.memory_card_delete_failed)
    val assignSuccessTemplate = stringResource(R.string.memory_card_assign_success)
    val assignFailureMessage = stringResource(R.string.memory_card_assign_failed)
    val slotClearedTemplate = stringResource(R.string.memory_card_slot_cleared)
    val restartRequiredMessage = stringResource(R.string.memory_card_needs_restart)
    val slotOneLabel = stringResource(R.string.memory_card_slot_1)
    val slotTwoLabel = stringResource(R.string.memory_card_slot_2)

    fun slotLabel(slot: Int): String = if (slot == 1) slotOneLabel else slotTwoLabel

    fun showCardMessage(message: String, affectsRunningGame: Boolean = false) {
        val warnAboutRunningGame = affectsRunningGame && EmulatorBridge.hasValidVm()
        Toast.makeText(
            context,
            if (warnAboutRunningGame) "$message $restartRequiredMessage" else message,
            Toast.LENGTH_LONG
        ).show()
    }

    fun clearedSlotsSuffix(clearedSlots: List<Int>): String =
        clearedSlots.joinToString(" ", prefix = " ") { slot ->
            memoryCardMessage(slotClearedTemplate, slotLabel(slot))
        }

    var cards by remember { mutableStateOf<List<MemoryCardInfo>>(emptyList()) }
    var assignments by remember { mutableStateOf(MemoryCardAssignments(slot1 = null, slot2 = null)) }
    var isLoading by remember { mutableStateOf(true) }
    var isWorking by remember { mutableStateOf(false) }
    val showCreateDialog = remember { mutableStateOf(false) }
    val pendingRename = remember { mutableStateOf<MemoryCardInfo?>(null) }
    val pendingDuplicate = remember { mutableStateOf<MemoryCardInfo?>(null) }
    val pendingDelete = remember { mutableStateOf<MemoryCardInfo?>(null) }
    val pendingExport = remember { mutableStateOf<MemoryCardInfo?>(null) }

    fun refresh() {
        scope.launch {
            isLoading = true
            val result = withContext(Dispatchers.IO) {
                val ensuredAssignments = repository.ensureDefaultCardsAssigned()
                repository.listCards() to ensuredAssignments
            }
            cards = result.first
            assignments = result.second
            isLoading = false
        }
    }

    LaunchedEffect(Unit) {
        refresh()
    }

    val backupLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/zip")
    ) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            isWorking = true
            val written = withContext(Dispatchers.IO) { repository.backupCards(cards, uri) }
            isWorking = false
            showCardMessage(
                if (written > 0) {
                    memoryCardMessage(backupSuccessTemplate, written)
                } else {
                    backupFailureMessage
                }
            )
            refresh()
        }
    }

    val restoreLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            isWorking = true
            val result = withContext(Dispatchers.IO) { repository.restoreCards(uri) }
            isWorking = false
            val message = when {
                !result.success || result.cards.isEmpty() -> restoreFailureMessage
                result.cards.size == 1 -> {
                    val single = result.cards.first()
                    if (single.overwritten) {
                        memoryCardMessage(importOverwrittenTemplate, single.name)
                    } else {
                        memoryCardMessage(importedTemplate, single.name)
                    }
                }

                else -> memoryCardMessage(
                    restoreSuccessTemplate,
                    result.cards.size,
                    result.createdCount,
                    result.overwrittenCount
                )
            }
            showCardMessage(message, affectsRunningGame = result.success)
            refresh()
        }
    }

    val exportLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri ->
        val card = pendingExport.value
        pendingExport.value = null
        if (uri == null || card == null) return@rememberLauncherForActivityResult
        scope.launch {
            isWorking = true
            val success = withContext(Dispatchers.IO) { repository.exportCard(card, uri) }
            isWorking = false
            showCardMessage(
                if (success) {
                    memoryCardMessage(exportSuccessTemplate, card.name)
                } else {
                    exportFailureMessage
                }
            )
            refresh()
        }
    }

    pendingDelete.value?.let { card ->
        AlertDialog(
            onDismissRequest = { pendingDelete.value = null },
            title = { Text(stringResource(R.string.memory_card_delete_confirm_title)) },
            text = {
                Text(
                    stringResource(
                        R.string.memory_card_delete_confirm_body,
                        card.name
                    )
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        scope.launch {
                            isWorking = true
                            val result = withContext(Dispatchers.IO) { repository.deleteCard(card) }
                            isWorking = false
                            pendingDelete.value = null
                            showCardMessage(
                                if (result.success) {
                                    memoryCardMessage(deleteSuccessTemplate, result.cardName.orEmpty()) +
                                        clearedSlotsSuffix(result.clearedSlots)
                                } else {
                                    deleteFailureMessage
                                },
                                affectsRunningGame = result.success && result.clearedSlots.isNotEmpty()
                            )
                            refresh()
                        }
                    }
                ) {
                    Text(stringResource(R.string.memory_card_delete_action))
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete.value = null }) {
                    Text(stringResource(android.R.string.cancel))
                }
            }
        )
    }

    if (showCreateDialog.value) {
        MemoryCardCreateDialog(
            title = stringResource(R.string.memory_card_create_title),
            confirmLabel = stringResource(R.string.memory_card_create_action),
            onDismiss = { showCreateDialog.value = false },
            onConfirm = { name ->
                scope.launch {
                    isWorking = true
                    val result = withContext(Dispatchers.IO) { repository.createPs1Card(name) }
                    isWorking = false
                    showCreateDialog.value = false
                    val message = when {
                        !result.success -> createFailureMessage
                        result.renamedToAvoidConflict -> memoryCardMessage(
                            createRenamedTemplate,
                            result.requestedName.orEmpty(),
                            result.cardName.orEmpty()
                        )

                        else -> memoryCardMessage(createSuccessTemplate, result.cardName.orEmpty())
                    }
                    showCardMessage(message)
                    refresh()
                }
            }
        )
    }

    pendingRename.value?.let { card ->
        MemoryCardNameDialog(
            title = stringResource(R.string.memory_card_rename_title),
            confirmLabel = stringResource(R.string.memory_card_rename_action),
            initialName = card.name.substringBeforeLast('.'),
            onDismiss = { pendingRename.value = null },
            onConfirm = { name ->
                scope.launch {
                    isWorking = true
                    val result = withContext(Dispatchers.IO) { repository.renameCard(card, name) }
                    isWorking = false
                    pendingRename.value = null
                    showCardMessage(
                        if (result.success) {
                            memoryCardMessage(
                                renameSuccessTemplate,
                                result.previousName ?: card.name,
                                result.cardName.orEmpty()
                            )
                        } else {
                            renameFailureMessage
                        }
                    )
                    refresh()
                }
            }
        )
    }

    pendingDuplicate.value?.let { card ->
        MemoryCardNameDialog(
            title = stringResource(R.string.memory_card_duplicate_title),
            confirmLabel = stringResource(R.string.memory_card_duplicate_action),
            initialName = card.name.substringBeforeLast('.') + " Copy",
            onDismiss = { pendingDuplicate.value = null },
            onConfirm = { name ->
                scope.launch {
                    isWorking = true
                    val result = withContext(Dispatchers.IO) { repository.duplicateCard(card, name) }
                    isWorking = false
                    pendingDuplicate.value = null
                    showCardMessage(
                        if (result.success) {
                            memoryCardMessage(
                                duplicateSuccessTemplate,
                                card.name,
                                result.cardName.orEmpty()
                            )
                        } else {
                            duplicateFailureMessage
                        }
                    )
                    refresh()
                }
            }
        )
    }

    val neonThemeActive = LocalNeonTheme.current
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontalSystemBarPadding)
    ) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                start = ScreenHorizontalPadding,
                end = ScreenHorizontalPadding,
                top = 0.dp,
                bottom = 24.dp + bottomInset
            ),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            item {
                MemoryCardHeader(
                    topInset = topInset,
                    isWorking = isWorking,
                    onBackClick = onBackClick
                )
            }

            if (neonThemeActive) {
                item {
                    NeonSystemBanner()
                }
            }

            item {
                FlowRow(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    FilledTonalButton(
                        shape = neonButtonShape(),
                        onClick = { showCreateDialog.value = true },
                        enabled = !isWorking,
                        colors = ButtonDefaults.filledTonalButtonColors(
                            containerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.18f),
                            contentColor = MaterialTheme.colorScheme.primary
                        )
                    ) {
                        Icon(Icons.Rounded.Add, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(stringResource(R.string.memory_card_create_action))
                    }
                    FilledTonalButton(
                        shape = neonButtonShape(),
                        onClick = { backupLauncher.launch("EmuCoreR-memory-cards.zip") },
                        enabled = cards.isNotEmpty() && !isWorking,
                        colors = ButtonDefaults.filledTonalButtonColors(
                            containerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.18f),
                            contentColor = MaterialTheme.colorScheme.primary
                        )
                    ) {
                        Icon(Icons.Rounded.Save, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(stringResource(R.string.memory_card_backup_action))
                    }
                    OutlinedButton(
                        shape = neonButtonShape(),
                        onClick = { restoreLauncher.launch(arrayOf("application/zip", "*/*")) },
                        enabled = !isWorking
                    ) {
                        Icon(Icons.Rounded.CloudDownload, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(stringResource(R.string.memory_card_restore_action))
                    }
                }
            }

            if (isLoading) {
                item {
                    LoadingCard()
                }
            } else if (cards.isEmpty()) {
                item {
                    EmptyMemoryCardsCard()
                }
            } else {
                items(cards, key = { it.path }) { card ->
                    MemoryCardItem(
                        card = card,
                        assignments = assignments,
                        onToggleSlot = { slot ->
                            scope.launch {
                                isWorking = true
                                val result = withContext(Dispatchers.IO) {
                                    val currentName = when (slot) {
                                        1 -> assignments.slot1
                                        else -> assignments.slot2
                                    }
                                    repository.assignCardToSlot(
                                        slot = slot,
                                        cardName = if (currentName.equals(card.name, ignoreCase = true)) null else card.name
                                    )
                                }
                                isWorking = false
                                val message = if (result.cardName.isNullOrBlank()) {
                                    memoryCardMessage(slotClearedTemplate, slotLabel(result.slot))
                                } else {
                                    memoryCardMessage(assignSuccessTemplate, result.cardName, slotLabel(result.slot)) +
                                        clearedSlotsSuffix(result.clearedSlots)
                                }
                                showCardMessage(
                                    if (result.success) message else assignFailureMessage,
                                    affectsRunningGame = result.success
                                )
                                refresh()
                            }
                        },
                        onExport = {
                            pendingExport.value = card
                            exportLauncher.launch(card.exportFileName())
                        },
                        onDuplicate = { pendingDuplicate.value = card },
                        onRename = { pendingRename.value = card },
                        onDelete = { pendingDelete.value = card }
                    )
                }
            }
        }
    }
}

@Composable
private fun MemoryCardHeader(
    topInset: androidx.compose.ui.unit.Dp,
    isWorking: Boolean,
    onBackClick: () -> Unit
) {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = topInset, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            ScreenTopBar(
                title = stringResource(R.string.memory_card_manager_title),
                onBackClick = onBackClick,
                modifier = Modifier.weight(1f)
            )
            if (isWorking) {
                CircularProgressIndicator(
                    modifier = Modifier.size(22.dp),
                    strokeWidth = 2.dp
                )
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MemoryCardItem(
    card: MemoryCardInfo,
    assignments: MemoryCardAssignments,
    onToggleSlot: (Int) -> Unit,
    onExport: () -> Unit,
    onDuplicate: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit
) {
    val assignedSlot1 = assignments.slot1.equals(card.name, ignoreCase = true)
    val assignedSlot2 = assignments.slot2.equals(card.name, ignoreCase = true)
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = neonShape(20.dp),
        tonalElevation = 1.dp,
        shadowElevation = 3.dp,
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(
            width = 1.dp,
            color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f)
        )
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.Top
            ) {
                Box(
                    modifier = Modifier
                        .size(54.dp)
                        .background(
                            color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.72f),
                            shape = neonShape(16.dp)
                        ),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = Icons.Rounded.Memory,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                }
                Column(
                    modifier = Modifier
                        .weight(1f)
                        .padding(start = 14.dp)
                ) {
                    Text(
                        text = card.name,
                        style = MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.SemiBold),
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    Text(
                        text = stringResource(
                            R.string.memory_card_meta,
                            card.storageLabel(),
                            DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
                                .format(Date(card.modifiedTime))
                        ),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        text = stringResource(
                            if (card.formatted) R.string.memory_card_state_formatted else R.string.memory_card_state_unformatted
                        ),
                        style = MaterialTheme.typography.labelMedium,
                        color = if (card.formatted) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.error
                        }
                    )
                }
            }

            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                FilterChip(
                    shape = neonChipShape(),
                    selected = assignedSlot1,
                    onClick = { onToggleSlot(1) },
                    label = { Text(stringResource(R.string.memory_card_slot_1)) }
                )
                FilterChip(
                    shape = neonChipShape(),
                    selected = assignedSlot2,
                    onClick = { onToggleSlot(2) },
                    label = { Text(stringResource(R.string.memory_card_slot_2)) }
                )
            }

            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                SmallActionButton(
                    icon = Icons.Rounded.Save,
                    label = stringResource(R.string.memory_card_export_action),
                    onClick = onExport
                )
                SmallActionButton(
                    icon = Icons.Rounded.ContentCopy,
                    label = stringResource(R.string.memory_card_duplicate_action),
                    onClick = onDuplicate
                )
                if (!card.isDefaultCard) {
                    SmallActionButton(
                        icon = Icons.Rounded.Edit,
                        label = stringResource(R.string.memory_card_rename_action),
                        onClick = onRename
                    )
                    SmallActionButton(
                        icon = Icons.Rounded.DeleteOutline,
                        label = stringResource(R.string.memory_card_delete_action),
                        onClick = onDelete
                    )
                }
            }
        }
    }
}

@Composable
private fun SmallActionButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    label: String,
    onClick: () -> Unit
) {
    OutlinedButton(shape = neonButtonShape(), onClick = onClick) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(16.dp))
        Spacer(modifier = Modifier.width(6.dp))
        Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun EmptyMemoryCardsCard() {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = neonShape(22.dp),
        color = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = stringResource(R.string.memory_card_empty_title),
                style = MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.Bold)
            )
            Text(
                text = stringResource(R.string.memory_card_empty_body),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun LoadingCard() {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = neonShape(22.dp),
        color = MaterialTheme.colorScheme.surface
    ) {
        Row(
            modifier = Modifier.padding(18.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            CircularProgressIndicator(modifier = Modifier.size(22.dp), strokeWidth = 2.dp)
            Text(
                text = stringResource(R.string.memory_card_loading),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 12.dp)
            )
        }
    }
}

@Composable
private fun MemoryCardCreateDialog(
    title: String,
    confirmLabel: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit
) {
    var value by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    text = stringResource(R.string.memory_card_type_file_desc),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                OutlinedTextField(
                    value = value,
                    onValueChange = { value = it },
                    singleLine = true,
                    shape = neonShape(18.dp),
                    label = { Text(stringResource(R.string.memory_card_name_field)) }
                )
            }
        },
        confirmButton = {
            TextButton(
                enabled = value.isNotBlank(),
                onClick = { value.trim().takeIf(String::isNotBlank)?.let(onConfirm) }
            ) { Text(confirmLabel) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.cancel)) }
        }
    )
}

@Composable
private fun MemoryCardNameDialog(
    title: String,
    confirmLabel: String,
    initialName: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit
) {
    var value by remember(initialName) { mutableStateOf(initialName) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = value,
                    onValueChange = { value = it },
                    singleLine = true,
                    shape = neonShape(18.dp),
                    label = { Text(stringResource(R.string.memory_card_name_field)) }
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    val trimmed = value.trim()
                    if (trimmed.isNotBlank()) {
                        onConfirm(trimmed)
                    }
                }
            ) {
                Text(confirmLabel)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        }
    )
}

@SuppressLint("DefaultLocale")
private fun formatBytes(bytes: Long): String {
    return when {
        bytes >= 1_073_741_824L -> String.format("%.2f GB", bytes / 1_073_741_824.0)
        bytes >= 1_048_576L -> String.format("%.1f MB", bytes / 1_048_576.0)
        bytes >= 1024L -> String.format("%.0f KB", bytes / 1024.0)
        else -> "$bytes B"
    }
}

private fun memoryCardMessage(template: String, vararg args: Any): String =
    String.format(Locale.getDefault(), template, *args)

@Composable
private fun MemoryCardInfo.storageLabel(): String {
    return stringResource(R.string.memory_card_file_size, formatBytes(sizeBytes))
}

private fun MemoryCardInfo.exportFileName(): String = name
