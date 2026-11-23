package com.sbro.emucorer.ui.common

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProPromoImpressionThrottleTest {
    private val minute = 60_000L

    @Test
    fun firstImpressionIsLoggedAndRepeatsWithinWindowAreNot() {
        val throttle = ProPromoImpressionThrottle(10 * minute)
        assertTrue(throttle.shouldLog("library", 1_000L))
        assertFalse(throttle.shouldLog("library", 1_000L + 9 * minute))
    }

    @Test
    fun loggedAgainOnceWindowElapsed() {
        val throttle = ProPromoImpressionThrottle(10 * minute)
        assertTrue(throttle.shouldLog("library", 1_000L))
        assertTrue(throttle.shouldLog("library", 1_000L + 10 * minute))
        assertFalse(throttle.shouldLog("library", 1_000L + 11 * minute))
    }

    @Test
    fun placementsAreThrottledIndependently() {
        val throttle = ProPromoImpressionThrottle(10 * minute)
        assertTrue(throttle.shouldLog("library", 1_000L))
        assertTrue(throttle.shouldLog("themes", 2_000L))
        assertFalse(throttle.shouldLog("themes", 3_000L))
    }

    @Test
    fun clockGoingBackwardsLogsAgain() {
        val throttle = ProPromoImpressionThrottle(10 * minute)
        assertTrue(throttle.shouldLog("library", 50 * minute))
        assertTrue(throttle.shouldLog("library", 1_000L))
    }
}
