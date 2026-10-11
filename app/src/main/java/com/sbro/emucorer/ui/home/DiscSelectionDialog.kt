package com.sbro.emucorer.ui.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sbro.emucorer.R
import com.sbro.emucorer.data.GameItem
import com.sbro.emucorer.ui.common.AppAlertDialog
import com.sbro.emucorer.ui.theme.neon.neonShape

internal data class DiscAction(val game: GameItem, val action: (GameItem) -> Unit)

@Composable
internal fun DiscSelectionDialog(game: GameItem, onDismiss: () -> Unit, onSelected: (GameItem) -> Unit) {
    AppAlertDialog(
        onDismissRequest = onDismiss,
        title = { Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(stringResource(R.string.home_choose_disc))
            Text(game.title, style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
        } },
        text = {
            LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 360.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(game.discs, key = { it.game.path }) { disc ->
                    FilledTonalButton(onClick = { onSelected(disc.game) },
                        modifier = Modifier.fillMaxWidth(), shape = neonShape(16.dp)) {
                        Column(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
                            verticalArrangement = Arrangement.spacedBy(3.dp)) {
                            Text(stringResource(R.string.home_disc_number, disc.number),
                                style = MaterialTheme.typography.titleSmall)
                            Text(disc.game.fileName, style = MaterialTheme.typography.bodySmall,
                                maxLines = 2, overflow = TextOverflow.Ellipsis)
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } }
    )
}

@Composable
internal fun DiscCountBadge(count: Int, modifier: Modifier = Modifier) {
    if (count < 2) return
    Surface(modifier = modifier, shape = neonShape(10.dp), color = MaterialTheme.colorScheme.primaryContainer) {
        Text(stringResource(R.string.home_disc_count, count), modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onPrimaryContainer)
    }
}
