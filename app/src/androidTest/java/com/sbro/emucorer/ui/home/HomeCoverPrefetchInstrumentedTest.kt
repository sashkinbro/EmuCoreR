package com.sbro.emucorer.ui.home

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyGridState
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.performScrollToIndex
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.sbro.emucorer.data.GameItem
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.awaitCancellation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class HomeCoverPrefetchInstrumentedTest {
    @get:Rule val compose = createComposeRule()

    @Test fun actualCardUsesThePrefetchedDefaultDecodeKeyAfterItsFileIsGone() {
        val context = androidx.test.platform.app.InstrumentationRegistry.getInstrumentation().targetContext
        val file = java.io.File.createTempFile("prefetch-cover-", ".png", context.cacheDir)
        val image = android.graphics.Bitmap.createBitmap(16, 16, android.graphics.Bitmap.Config.ARGB_8888)
        try {
            file.outputStream().use { assertTrue(image.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)) }
            kotlinx.coroutines.runBlocking { com.sbro.emucorer.ui.common.prefetchGameCoverArt(context, file.path) }
            assertTrue(file.delete())
            compose.setContent {
                com.sbro.emucorer.ui.common.GameCoverArt(file.path, "Warmed cover", loadEnabled = false)
            }
            compose.onNodeWithContentDescription("Warmed cover").assertExists()
        } finally {
            image.recycle()
            file.delete()
            com.sbro.emucorer.ui.common.clearCoverImageMemoryCache()
        }
    }

    @Test fun actualLazyLibraryCancelsOldLookaheadAndReprioritizesAfterReversal() {
        val fixture = Fixture()
        fixture.show()
        compose.waitUntil { fixture.active.get() == 1 }
        compose.onNodeWithTag("library").performScrollToIndex(1000)
        fixture.assertCurrentWindow(forward = true)
        compose.onNodeWithTag("library").performScrollToIndex(900)
        fixture.assertCurrentWindow(forward = false)
        assertEquals("More than one speculative decode ran together", 1, fixture.maximumActive.get())
        assertTrue("A scroll enqueued the catalog instead of a small window", fixture.started.get() < 20)
        assertTrue("Stale speculative requests were retained", fixture.cancelled.get() >= 2)
    }

    @Test fun disablingLibraryLookaheadCancelsItsPendingDecode() {
        val fixture = Fixture()
        fixture.show()
        compose.waitUntil { fixture.active.get() == 1 }
        compose.runOnIdle { fixture.enabled.value = false }
        compose.waitUntil { fixture.active.get() == 0 }
        val before = fixture.started.get()
        compose.onNodeWithTag("library").performScrollToIndex(1000)
        compose.runOnIdle { assertEquals(before, fixture.started.get()) }
    }

    private inner class Fixture {
        val enabled = mutableStateOf(true)
        val active = AtomicInteger()
        val maximumActive = AtomicInteger()
        val started = AtomicInteger()
        val cancelled = AtomicInteger()
        val current = AtomicReference<String?>(null)
        lateinit var gridState: LazyGridState
        val games = List(10000) { GameItem("Game $it", "/game/$it", "$it.3ds", 1, 0) }
        val window = HomeCoverPrefetchWindow(games.map {
            HomeCoverPrefetchRow(homeLibraryRowKey(it), listOf("cover_${it.path}"))
        })
        private val load: suspend (String) -> Unit = { path ->
            val running = active.incrementAndGet()
            maximumActive.updateAndGet { maxOf(it, running) }
            started.incrementAndGet()
            current.set(path)
            try {
                // Hold the request so a real lazy-scroll update must cancel it.
                awaitCancellation()
            } finally {
                current.set(null)
                active.decrementAndGet()
                cancelled.incrementAndGet()
            }
        }

        fun show() {
            compose.setContent {
                gridState = rememberLazyGridState()
                HomeCoverPrefetch(gridState, window, enabled.value, load)
                LazyVerticalGrid(
                    GridCells.Fixed(1), Modifier.height(300.dp).testTag("library"), state = gridState
                ) {
                    homeLibraryItems(games, 1, true) { _, _ ->
                        Box(Modifier.fillMaxWidth().height(100.dp))
                    }
                }
            }
        }

        fun assertCurrentWindow(forward: Boolean) {
            var expected: String? = null
            compose.runOnIdle {
                expected = window.paths(gridState.layoutInfo.visibleItemsInfo.map { it.key.toString() }, forward).first()
            }
            compose.waitUntil { current.get() == expected }
            compose.runOnIdle { assertEquals(expected, current.get()) }
        }
    }
}
