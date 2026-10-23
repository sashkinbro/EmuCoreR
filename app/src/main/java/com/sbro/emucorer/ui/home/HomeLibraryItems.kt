package com.sbro.emucorer.ui.home

import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.runtime.Composable
import com.sbro.emucorer.data.GameItem

internal fun homeLibraryRowKey(game: GameItem): String = "library_${game.path}"

internal fun LazyGridScope.homeLibraryItems(
    games: List<GameItem>,
    columns: Int,
    listView: Boolean,
    row: @Composable (List<GameItem>, Int) -> Unit
) {
    val count = if (listView) 1 else columns.coerceAtLeast(1)
    // Register rows individually so offscreen cards do not compose or load covers.
    items(
        count = (games.size + count - 1) / count,
        key = { index -> homeLibraryRowKey(games[index * count]) },
        contentType = { if (listView) "list" else "covers_$count" },
        span = { GridItemSpan(maxLineSpan) }
    ) { index ->
        val start = index * count
        row(games.subList(start, minOf(start + count, games.size)), count)
    }
}
