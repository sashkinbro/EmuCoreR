package com.sbro.emucorer.ui.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class HomeCoverPrefetchWindowTest {
    private fun rows(count: Int, columns: Int = 2) = (0 until count).map { row ->
        HomeCoverPrefetchRow("row_$row", (0 until columns).map { "cover_${row}_$it" })
    }

    @Test fun forwardScrollWarmsTwoNearestRowsBeforeTheTrailingRow() {
        val window = HomeCoverPrefetchWindow(rows(10))
        assertEquals(
            listOf("cover_5_0", "cover_5_1", "cover_6_0", "cover_6_1", "cover_2_0", "cover_2_1"),
            window.paths(listOf("search", "row_3", "row_4"), forward = true)
        )
    }

    @Test fun reversingScrollPrioritizesTheNewDirectionAndNeverRequestsTheViewport() {
        val window = HomeCoverPrefetchWindow(rows(10))
        assertEquals(
            listOf("cover_2_0", "cover_2_1", "cover_1_0", "cover_1_1", "cover_5_0", "cover_5_1"),
            window.paths(listOf("row_4", "row_3"), forward = false)
        )
    }

    @Test fun wideGridsHaveAtMostEightDistinctSpeculativeImagesNearestFirst() {
        val window = HomeCoverPrefetchWindow(rows(20, columns = 7))
        val paths = window.paths(listOf("row_5", "row_6"), forward = true)
        assertEquals((0..6).map { "cover_7_$it" } + "cover_8_0", paths)
        assertEquals(8, paths.distinct().size)
    }

    @Test fun headerOnlyViewportWarmsInitialRowsWithoutTreatingHeadersAsGames() {
        val window = HomeCoverPrefetchWindow(rows(8))
        assertEquals(
            listOf("cover_0_0", "cover_0_1", "cover_1_0", "cover_1_1"),
            window.paths(listOf("header", "platform_tabs", "recent"), forward = true)
        )
    }

    @Test fun listAndPartialLastRowsStayWithinBoundsInBothDirections() {
        val window = HomeCoverPrefetchWindow(rows(4, columns = 1))
        assertEquals(listOf("cover_3_0", "cover_1_0"), window.paths(listOf("row_2"), true))
        assertEquals(listOf("cover_2_0", "cover_1_0"), window.paths(listOf("row_3"), false))
        assertTrue(HomeCoverPrefetchWindow(emptyList()).paths(listOf("header"), true).isEmpty())
    }

    @Test fun missingDisabledDuplicateAndVisibleArtworkNeverConsumesPrefetchSlots() {
        val window = HomeCoverPrefetchWindow(listOf(
            HomeCoverPrefetchRow("row_0", listOf("visible", "same")),
            HomeCoverPrefetchRow("row_1", listOf(null, "", " ", "same", "new", "new")),
            HomeCoverPrefetchRow("row_2", listOf("another"))
        ))
        assertEquals(listOf("new", "another"), window.paths(listOf("row_0"), true))
    }
}
