package com.sbro.emucorer.ui.common

import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.data.OverlayControlLayout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OverlayCanvasResponsiveLayoutTest {
    private val density = Density(3f)

    @Test
    fun `default controls fit without overlap on common screen shapes`() {
        listOf(
            ScreenCase("narrow landscape phone", 640.dp, 280.dp),
            ScreenCase("16 by 9 phone", 800.dp, 450.dp),
            ScreenCase("20 by 9 phone", 900.dp, 405.dp),
            ScreenCase("4 by 3 tablet landscape", 1024.dp, 768.dp),
            ScreenCase("4 by 3 tablet portrait", 768.dp, 1024.dp)
        ).forEach { screen ->
            assertResponsiveLayout(screen)
        }
    }

    @Test
    fun `default layout keeps the independent dpad beside the left stick`() {
        // The right stick is hidden by default in this build, so show it explicitly to
        // compare the primary control gaps on both sides.
        val controls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["right_stick"] = requireNotNull(this["right_stick"]).copy(visible = true)
        }
        listOf(
            ScreenCase("narrow landscape phone", 640.dp, 280.dp),
            ScreenCase("20 by 9 phone", 900.dp, 405.dp),
            ScreenCase("4 by 3 tablet landscape", 1024.dp, 768.dp)
        ).forEach { screen ->
            val layout = buildLayout(screen, controls)
            val dpad = requireNotNull(layout.dpadCluster)
            val leftStick = requireNotNull(layout.leftStick)
            val rightStick = requireNotNull(layout.rightStick)
            val leftGap = leftStick.x - (dpad.x + dpad.size)
            val rightGap = layout.actionButtons.minOf { it.x } - (rightStick.x + rightStick.size)
            val leftShoulderBottom = layout.leftShoulders.maxOf { it.y + it.height }

            assertTrue("${screen.name}: independent dpad must be visible", dpad.visible)
            assertTrue("${screen.name}: left stick must remain visible", leftStick.visible)
            assertTrue("${screen.name}: dpad must stay left of the left stick", dpad.x + dpad.size < leftStick.x)
            assertTrue("${screen.name}: left shoulders must stay above the dpad", leftShoulderBottom < dpad.y)
            assertEquals("${screen.name}: primary control gaps must match", leftGap.value, rightGap.value, EPSILON)
        }
    }

    @Test
    fun `explicitly hidden independent dpad remains hidden`() {
        val controls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["dpad_cluster"] = requireNotNull(this["dpad_cluster"]).copy(visible = false)
        }

        val layout = buildLayout(ScreenCase("custom hidden dpad", 800.dp, 450.dp), controls)

        assertTrue(layout.dpadCluster == null)
        assertTrue(layout.leftStick?.visible == true)
    }

    @Test
    fun `asymmetric cutout and navigation insets define the actual safe area`() {
        val screen = ScreenCase(
            name = "asymmetric insets",
            width = 720.dp,
            height = 360.dp,
            leftInset = 54.dp,
            rightInset = 18.dp,
            topInset = 28.dp,
            bottomInset = 32.dp
        )

        val layout = buildLayout(screen)
        assertInsideSafeArea(screen, layout)
        assertNoOverlaps(screen.name, layout)

        val left = requireNotNull(layout.leftStick)
        val independentDpad = requireNotNull(layout.dpadCluster)
        val rightActionEdge = layout.actionButtons.maxOf { it.x + it.width }
        assertTrue(left.x >= screen.leftInset)
        assertTrue(rightActionEdge <= screen.width - screen.rightInset)
        val leftMargin = independentDpad.x.value
        val rightMargin = (screen.width - rightActionEdge).value
        assertEquals(
            "both sides must keep the same edge padding",
            leftMargin - screen.leftInset.value,
            rightMargin - screen.rightInset.value,
            EPSILON
        )
        assertTrue("the cutout side must keep the larger margin", leftMargin > rightMargin)

        val leftTopEdge = layout.leftShoulders.minOf { it.x }
        val rightShoulderEdge = layout.rightShoulders.maxOf { it.x + it.width }
        assertEquals(
            "both shoulder rows must keep the same edge padding",
            leftTopEdge.value - screen.leftInset.value,
            (screen.width - rightShoulderEdge).value - screen.rightInset.value,
            EPSILON
        )

        val centerLeft = layout.centerButtons.minOf { it.x }
        val centerRight = layout.centerButtons.maxOf { it.x + it.width }
        assertEquals(
            "center controls must stay on the physical screen center",
            screen.width.value / 2f,
            ((centerLeft + centerRight) / 2f).value,
            EPSILON
        )
    }

    @Test
    fun `visible right stick is placed without colliding in landscape and portrait`() {
        val controls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["right_stick"] = requireNotNull(this["right_stick"]).copy(visible = true)
        }
        listOf(
            ScreenCase("right stick landscape", 640.dp, 280.dp),
            ScreenCase("right stick portrait", 360.dp, 800.dp),
            ScreenCase("right stick tablet", 1024.dp, 768.dp)
        ).forEach { screen ->
            val layout = buildLayout(screen, controls)
            assertInsideSafeArea(screen, layout)
            assertNoOverlaps(screen.name, layout)
        }
    }

    @Test
    fun `wide analog touch surfaces participate in responsive spacing`() {
        val screen = ScreenCase("wide analog surfaces", 640.dp, 280.dp)
        val controls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["left_stick"] = requireNotNull(this["left_stick"]).copy(
                surfaceOnly = true,
                widthScale = 200
            )
            this["right_stick"] = requireNotNull(this["right_stick"]).copy(
                visible = true,
                surfaceOnly = true,
                widthScale = 200
            )
        }
        val layout = buildLayout(screen, controls)
        val left = requireNotNull(layout.leftStick)
        val right = requireNotNull(layout.rightStick)
        val leftPanelLeft = left.x - (left.size * (left.widthScale / 100f) - left.size) / 2f
        val leftPanelRight = leftPanelLeft + left.size * (left.widthScale / 100f)
        val rightPanelLeft = right.x - (right.size * (right.widthScale / 100f) - right.size) / 2f
        val rightPanelRight = rightPanelLeft + right.size * (right.widthScale / 100f)
        val actionLeft = layout.actionButtons.minOf { it.x }

        assertTrue(leftPanelLeft >= screen.leftInset)
        assertTrue(leftPanelRight < rightPanelLeft)
        assertTrue(rightPanelRight < actionLeft)
    }

    @Test
    fun `custom per-control offsets remain relative to responsive anchors`() {
        val screen = ScreenCase("custom", 800.dp, 450.dp)
        val defaults = buildLayout(screen)
        val customControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["triangle"] = requireNotNull(this["triangle"]).copy(offset = 33f to -18f)
        }
        val customized = buildLayout(screen, customControls)
        val defaultTriangle = requireNotNull(defaults.button("triangle"))
        val customTriangle = requireNotNull(customized.button("triangle"))

        assertEquals(11f, (customTriangle.x - defaultTriangle.x).value, 0.001f)
        assertEquals(-6f, (customTriangle.y - defaultTriangle.y).value, 0.001f)
    }

    @Test
    fun `large requested scale is reduced only where the safe area requires it`() {
        listOf(
            ScreenCase("large scale narrow", 640.dp, 280.dp),
            ScreenCase("large scale inset", 720.dp, 360.dp, 48.dp, 24.dp, 20.dp, 30.dp),
            ScreenCase("large scale tablet", 1024.dp, 768.dp)
        ).forEach { screen ->
            val layout = buildLayout(screen, overlayScale = 1.6f)
            assertInsideSafeArea(screen, layout)
            assertNoOverlaps(screen.name, layout)
        }
    }

    @Test
    fun `legacy group positions remain as deltas from responsive defaults`() {
        val screen = ScreenCase("group offset", 800.dp, 450.dp)
        val defaults = buildLayout(screen)
        val moved = buildLayout(
            screen = screen,
            actionOffset = (AppPreferences.DEFAULT_ACTION_OFFSET_X + 30f) to
                (AppPreferences.DEFAULT_ACTION_OFFSET_Y - 15f)
        )
        val defaultTriangle = requireNotNull(defaults.button("triangle"))
        val movedTriangle = requireNotNull(moved.button("triangle"))

        assertEquals(10f, (movedTriangle.x - defaultTriangle.x).value, 0.001f)
        assertEquals(-5f, (movedTriangle.y - defaultTriangle.y).value, 0.001f)
    }

    @Test
    fun `default dpad cluster arrows keep their in-block slots`() {
        val layout = buildLayout(ScreenCase("default dpad arrows", 800.dp, 450.dp))
        val dpad = requireNotNull(layout.dpadCluster)

        OverlayDpadDirection.entries.forEach { direction ->
            val offset = requireNotNull(dpad.directionOffsets[direction])
            assertEquals(0f, offset.x.value, EPSILON)
            assertEquals(0f, offset.y.value, EPSILON)
        }
        assertEquals(0f, dpad.surface.offset.x.value, EPSILON)
        assertEquals(0f, dpad.surface.offset.y.value, EPSILON)
        assertEquals(dpad.size.value, dpad.surface.width.value, EPSILON)
        assertEquals(dpad.size.value, dpad.surface.height.value, EPSILON)
    }

    @Test
    fun `dpad cluster arrows follow per-control offsets`() {
        val screen = ScreenCase("dpad arrow offsets", 800.dp, 450.dp)
        val customControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["dpad_up"] = requireNotNull(this["dpad_up"]).copy(offset = 0f to 9f)
            this["dpad_right"] = requireNotNull(this["dpad_right"]).copy(offset = -12f to 0f)
        }
        val layout = buildLayout(screen, customControls)
        val dpad = requireNotNull(layout.dpadCluster)

        assertEquals(3f, dpad.directionOffsets.getValue(OverlayDpadDirection.Up).y.value, EPSILON)
        assertEquals(0f, dpad.directionOffsets.getValue(OverlayDpadDirection.Up).x.value, EPSILON)
        assertEquals(-4f, dpad.directionOffsets.getValue(OverlayDpadDirection.Right).x.value, EPSILON)
        assertEquals(0f, dpad.directionOffsets.getValue(OverlayDpadDirection.Right).y.value, EPSILON)
    }

    @Test
    fun `dpad cluster surface grows around arrows moved outside the base square`() {
        val screen = ScreenCase("dpad surface", 800.dp, 450.dp)
        val customControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["dpad_left"] = requireNotNull(this["dpad_left"]).copy(offset = -30f to 0f)
            this["dpad_right"] = requireNotNull(this["dpad_right"]).copy(offset = 0f to 300f)
        }
        val layout = buildLayout(screen, customControls)
        val dpad = requireNotNull(layout.dpadCluster)
        val surface = dpad.surface
        val arrowSize = dpad.size * OverlayDpadClusterArrowScale

        assertTrue(surface.offset.x < 0.dp)
        assertTrue(surface.width > dpad.size)
        assertTrue(surface.height > dpad.size)
        OverlayDpadDirection.entries.forEach { direction ->
            val anchor = overlayDpadArrowDefault(direction, dpad.size) +
                dpad.directionOffsets.getValue(direction)
            assertTrue(anchor.x.value >= surface.offset.x.value - EPSILON)
            assertTrue(anchor.y.value >= surface.offset.y.value - EPSILON)
            assertTrue(
                (anchor.x + arrowSize).value <=
                    (surface.offset.x + surface.width).value + EPSILON
            )
            assertTrue(
                (anchor.y + arrowSize).value <=
                    (surface.offset.y + surface.height).value + EPSILON
            )
        }
    }

    @Test
    fun `hiding the independent dpad keeps the left stick in place`() {
        val screen = ScreenCase("stable left stick", 800.dp, 450.dp)
        val defaults = buildLayout(screen)
        val hiddenControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["dpad_cluster"] = requireNotNull(this["dpad_cluster"]).copy(visible = false)
        }
        val hidden = buildLayout(screen, hiddenControls)
        val defaultStick = requireNotNull(defaults.leftStick)
        val hiddenStick = requireNotNull(hidden.leftStick)

        assertTrue(hidden.dpadCluster?.visible != true)
        assertEquals(defaultStick.x.value, hiddenStick.x.value, EPSILON)
        assertEquals(defaultStick.y.value, hiddenStick.y.value, EPSILON)
    }

    @Test
    fun `stick toggle dpad shares the stick position and hides the automatic dpad`() {
        val screen = ScreenCase("toggle dpad", 800.dp, 450.dp)
        val visibleStickControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["left_stick"] = requireNotNull(this["left_stick"]).copy(offset = 40f to -20f)
        }
        val toggledControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["dpad_cluster"] = requireNotNull(this["dpad_cluster"]).copy(visible = false)
            this["left_stick"] = requireNotNull(this["left_stick"]).copy(
                visible = false,
                offset = 40f to -20f
            )
            this["dpad_toggle"] = requireNotNull(this["dpad_toggle"]).copy(visible = true)
        }

        val visibleLayout = buildLayout(screen, visibleStickControls)
        val toggledLayout = buildLayout(screen, toggledControls)
        val stick = requireNotNull(visibleLayout.leftStick)
        val toggleDpad = requireNotNull(toggledLayout.toggleDpad)

        assertEquals("left_stick", toggleDpad.replacesStickId)
        assertEquals(stick.x.value, toggleDpad.x.value, EPSILON)
        assertEquals(stick.y.value, toggleDpad.y.value, EPSILON)
        assertTrue("automatic dpad must stay hidden while the toggle dpad is active", toggledLayout.dpadButtons.isEmpty())
    }

    @Test
    fun `scaling the extra dpad does not move the left stick`() {
        val screen = ScreenCase("scaled extra dpad", 800.dp, 450.dp)
        val defaults = buildLayout(
            screen,
            AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
                this["dpad_cluster"] = requireNotNull(this["dpad_cluster"]).copy(visible = false)
            }
        )
        val scaled = buildLayout(
            screen,
            AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
                this["dpad_cluster"] = requireNotNull(this["dpad_cluster"]).copy(scale = 220)
            }
        )
        val defaultStick = requireNotNull(defaults.leftStick)
        val scaledStick = requireNotNull(scaled.leftStick)

        assertEquals(defaultStick.x.value, scaledStick.x.value, EPSILON)
        assertEquals(defaultStick.y.value, scaledStick.y.value, EPSILON)
    }

    @Test
    fun `scaling the left stick does not move the dpad or action clusters`() {
        val screen = ScreenCase("scaled stick", 800.dp, 450.dp)
        val defaults = buildLayout(screen)
        val scaled = buildLayout(
            screen,
            AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
                this["left_stick"] = requireNotNull(this["left_stick"]).copy(scale = 200)
            }
        )
        val defaultDpad = requireNotNull(defaults.dpadCluster)
        val scaledDpad = requireNotNull(scaled.dpadCluster)
        val defaultTriangle = requireNotNull(defaults.button("triangle"))
        val scaledTriangle = requireNotNull(scaled.button("triangle"))

        assertEquals(defaultDpad.x.value, scaledDpad.x.value, EPSILON)
        assertEquals(defaultDpad.y.value, scaledDpad.y.value, EPSILON)
        assertEquals(defaultTriangle.x.value, scaledTriangle.x.value, EPSILON)
        assertEquals(defaultTriangle.y.value, scaledTriangle.y.value, EPSILON)
    }

    @Test
    fun `scaling a centre button keeps its neighbours on their slots`() {
        val screen = ScreenCase("scaled centre button", 800.dp, 450.dp)
        // l3/r3 are hidden in this build by default, so show them to compare all
        // centre-button slots while one of their neighbours is scaled.
        val visibleCenterControls = AppPreferences.defaultOverlayControlLayouts().toMutableMap().apply {
            this["l3"] = requireNotNull(this["l3"]).copy(visible = true)
            this["r3"] = requireNotNull(this["r3"]).copy(visible = true)
        }
        val defaults = buildLayout(screen, visibleCenterControls)
        val scaled = buildLayout(
            screen,
            visibleCenterControls.toMutableMap().apply {
                this["left_input_toggle"] = requireNotNull(this["left_input_toggle"]).copy(scale = 180)
            }
        )
        listOf("select", "start", "l3", "r3").forEach { id ->
            val defaultButton = requireNotNull(defaults.button(id))
            val scaledButton = requireNotNull(scaled.button(id))
            assertEquals("$id must keep its slot", defaultButton.x.value, scaledButton.x.value, EPSILON)
        }
    }

    private fun assertResponsiveLayout(screen: ScreenCase) {
        val layout = buildLayout(screen)
        assertInsideSafeArea(screen, layout)
        assertNoOverlaps(screen.name, layout)
    }

    private fun buildLayout(
        screen: ScreenCase,
        controls: Map<String, OverlayControlLayout> = AppPreferences.defaultOverlayControlLayouts(),
        overlayScale: Float = 1f,
        actionOffset: Pair<Float, Float> =
            AppPreferences.DEFAULT_ACTION_OFFSET_X to AppPreferences.DEFAULT_ACTION_OFFSET_Y
    ): OverlayCanvasLayout = buildOverlayCanvasLayout(
        canvasWidth = screen.width,
        canvasHeight = screen.height,
        density = density,
        scaleFactor = overlayScale,
        stickScaleFactor = 1f,
        dpadOffset = AppPreferences.DEFAULT_DPAD_OFFSET_X to AppPreferences.DEFAULT_DPAD_OFFSET_Y,
        lstickOffset = AppPreferences.DEFAULT_LSTICK_OFFSET_X to AppPreferences.DEFAULT_LSTICK_OFFSET_Y,
        rstickOffset = AppPreferences.DEFAULT_RSTICK_OFFSET_X to AppPreferences.DEFAULT_RSTICK_OFFSET_Y,
        actionOffset = actionOffset,
        lbtnOffset = AppPreferences.DEFAULT_LBTN_OFFSET_X to AppPreferences.DEFAULT_LBTN_OFFSET_Y,
        rbtnOffset = AppPreferences.DEFAULT_RBTN_OFFSET_X to AppPreferences.DEFAULT_RBTN_OFFSET_Y,
        centerOffset = AppPreferences.DEFAULT_CENTER_OFFSET_X to AppPreferences.DEFAULT_CENTER_OFFSET_Y,
        controlLayouts = controls,
        safeLeftInset = screen.leftInset,
        safeRightInset = screen.rightInset,
        safeTopInset = screen.topInset,
        safeBottomInset = screen.bottomInset
    )

    private fun assertInsideSafeArea(screen: ScreenCase, layout: OverlayCanvasLayout) {
        val bounds = bounds(layout)
        bounds.forEach { item ->
            assertTrue("${screen.name}: ${item.id} crosses left inset", item.left >= screen.leftInset.value - EPSILON)
            assertTrue("${screen.name}: ${item.id} crosses top inset", item.top >= screen.topInset.value - EPSILON)
            assertTrue(
                "${screen.name}: ${item.id} crosses right inset",
                item.right <= (screen.width - screen.rightInset).value + EPSILON
            )
            assertTrue(
                "${screen.name}: ${item.id} crosses bottom inset",
                item.bottom <= (screen.height - screen.bottomInset).value + EPSILON
            )
        }
    }

    private fun assertNoOverlaps(name: String, layout: OverlayCanvasLayout) {
        val items = bounds(layout)
        for (firstIndex in items.indices) {
            for (secondIndex in firstIndex + 1 until items.size) {
                val first = items[firstIndex]
                val second = items[secondIndex]
                val overlaps = first.left < second.right - EPSILON &&
                    first.right > second.left + EPSILON &&
                    first.top < second.bottom - EPSILON &&
                    first.bottom > second.top + EPSILON
                assertFalse("$name: ${first.id} overlaps ${second.id}", overlaps)
            }
        }
    }

    private fun bounds(layout: OverlayCanvasLayout): List<Bounds> = buildList {
        layout.allButtons.filter { it.visible }.forEach {
            add(Bounds(it.id, it.x.value, it.y.value, (it.x + it.width).value, (it.y + it.height).value))
        }
        layout.leftStick?.takeIf { it.visible }?.let {
            add(Bounds(it.id, it.x.value, it.y.value, (it.x + it.size).value, (it.y + it.size).value))
        }
        layout.rightStick?.takeIf { it.visible }?.let {
            add(Bounds(it.id, it.x.value, it.y.value, (it.x + it.size).value, (it.y + it.size).value))
        }
        layout.dpadCluster?.takeIf { it.visible }?.let {
            add(Bounds(it.id, it.x.value, it.y.value, (it.x + it.size).value, (it.y + it.size).value))
        }
    }

    private data class ScreenCase(
        val name: String,
        val width: Dp,
        val height: Dp,
        val leftInset: Dp = 0.dp,
        val rightInset: Dp = 0.dp,
        val topInset: Dp = 0.dp,
        val bottomInset: Dp = 0.dp
    )

    private data class Bounds(
        val id: String,
        val left: Float,
        val top: Float,
        val right: Float,
        val bottom: Float
    )

    private companion object {
        const val EPSILON = 0.01f
    }
}
