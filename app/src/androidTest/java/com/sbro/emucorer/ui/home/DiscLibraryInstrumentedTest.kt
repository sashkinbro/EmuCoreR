package com.sbro.emucorer.ui.home

import android.graphics.Bitmap
import android.os.Build
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.isDialog
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performClick
import androidx.compose.ui.unit.dp
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorer.R
import com.sbro.emucorer.data.GameItem
import com.sbro.emucorer.data.groupLibraryDiscs
import java.io.File
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

class DiscLibraryInstrumentedTest {
    @get:Rule val compose = createComposeRule()
    private val first = GameItem("Oddworld — Abe’s Exoddus", "/owned/disc1.chd", "Oddworld - Abe's Exoddus (USA) (Disc 1).chd", 100, 1)
    private val second = first.copy(path = "/owned/disc2.chd", fileName = "Oddworld - Abe's Exoddus (USA) (Disc 2).chd", fileSize = 200)

    @Test fun choosingSecondDiscDeliversItsOriginalLaunchMetadata() {
        assertEquals("OnePlus", Build.MANUFACTURER)
        assertEquals("CPH2747", Build.MODEL)
        var selected: GameItem? = null
        var visible by mutableStateOf(true)
        val group = groupLibraryDiscs(listOf(second, first)).single()
        compose.setContent { MaterialTheme {
            if (visible) DiscSelectionDialog(group, onDismiss = { visible = false }, onSelected = {
                selected = it
                visible = false
            })
        } }
        compose.onNodeWithText(first.fileName).assertExists()
        compose.onNodeWithText(second.fileName).assertExists()
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val preview = File(context.cacheDir, "disc-library-dialog.png")
        compose.onNode(isDialog()).captureToImage().asAndroidBitmap().let { bitmap ->
            preview.outputStream().use { assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) }
        }
        compose.onNodeWithText(second.fileName).performClick()
        compose.runOnIdle {
            assertFalse(visible)
            assertEquals(second, selected)
            assertTrue(selected!!.discs.isEmpty())
        }
    }

    @Test fun cancellingDiscSelectionDoesNotLaunchAnything() {
        var selected: GameItem? = null
        var dismissed by mutableStateOf(false)
        val group = groupLibraryDiscs(listOf(first, second)).single()
        compose.setContent { MaterialTheme {
            if (!dismissed) DiscSelectionDialog(group, onDismiss = { dismissed = true }, onSelected = { selected = it })
        } }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        compose.onNodeWithText(context.getString(R.string.cancel)).performClick()
        compose.runOnIdle { assertTrue(dismissed); assertNull(selected) }
    }

    @Test fun libraryOptionsExposeGroupingAndSortingWithoutSearch() {
        var grouped by mutableStateOf(true)
        var sort by mutableStateOf(HomeSortOption.TITLE_ASC)
        var refreshRequested = false
        compose.setContent { MaterialTheme {
            HomeHeader(gamesCount = 1, isWide = false, isLandscape = false, horizontalInset = 16.dp,
                sectionTopSpacing = 2.dp, isRefreshing = false, refreshEnabled = true, isShelfView = false,
                standardViewMode = HomeLibraryViewMode.GRID, groupDiscs = grouped, sortOption = sort,
                onToggleDiscGrouping = { grouped = !grouped }, onSortSelected = { sort = it },
                onRefresh = { refreshRequested = true }, onToggleShelfMode = {}, onToggleLibraryViewMode = {}, onMenuClick = {})
        } }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        compose.onRoot().captureToImage().asAndroidBitmap().let { bitmap ->
            File(context.cacheDir, "disc-library-header.png").outputStream().use {
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
        }
        compose.onNodeWithContentDescription(context.getString(R.string.home_library_options)).performClick()
        compose.onNodeWithText(context.getString(R.string.home_group_discs)).performClick()
        compose.runOnIdle { assertFalse(grouped) }
        compose.onNodeWithContentDescription(context.getString(R.string.home_library_options)).performClick()
        compose.onNodeWithText(context.getString(R.string.home_sort_size_desc)).performClick()
        compose.runOnIdle { assertEquals(HomeSortOption.SIZE_DESC, sort) }
        compose.onNodeWithContentDescription(context.getString(R.string.home_library_options)).performClick()
        compose.onNodeWithText(context.getString(R.string.home_refresh)).performClick()
        compose.runOnIdle { assertTrue(refreshRequested) }
    }
}
