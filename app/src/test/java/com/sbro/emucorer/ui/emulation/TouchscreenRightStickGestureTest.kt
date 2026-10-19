package com.sbro.emucorer.ui.emulation

import org.junit.Assert.assertEquals
import org.junit.Test

class TouchscreenRightStickGestureTest {

    @Test
    fun `disabled touchscreen sticks ignore empty space drags`() {
        assertEquals(
            TouchscreenStickGestureTarget.NONE,
            resolveTouchscreenStickGestureTarget(
                leftEnabled = false,
                rightEnabled = false,
                startX = 10f,
                widthPx = 100f
            )
        )
    }

    @Test
    fun `single enabled stick uses the whole free area`() {
        assertEquals(
            TouchscreenStickGestureTarget.RIGHT,
            resolveTouchscreenStickGestureTarget(
                leftEnabled = false,
                rightEnabled = true,
                startX = 10f,
                widthPx = 100f
            )
        )
        assertEquals(
            TouchscreenStickGestureTarget.LEFT,
            resolveTouchscreenStickGestureTarget(
                leftEnabled = true,
                rightEnabled = false,
                startX = 90f,
                widthPx = 100f
            )
        )
    }

    @Test
    fun `both enabled sticks split the free area in half`() {
        assertEquals(
            TouchscreenStickGestureTarget.LEFT,
            resolveTouchscreenStickGestureTarget(
                leftEnabled = true,
                rightEnabled = true,
                startX = 49f,
                widthPx = 100f
            )
        )
        assertEquals(
            TouchscreenStickGestureTarget.RIGHT,
            resolveTouchscreenStickGestureTarget(
                leftEnabled = true,
                rightEnabled = true,
                startX = 50f,
                widthPx = 100f
            )
        )
    }
}
