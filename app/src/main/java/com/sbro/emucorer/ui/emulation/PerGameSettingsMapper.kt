package com.sbro.emucorer.ui.emulation

import com.sbro.emucorer.data.CustomTouchControlLibrary
import com.sbro.emucorer.data.TouchControlsLayoutProfile

internal fun EmulationUiState.withTouchControlsLayout(
    profile: TouchControlsLayoutProfile
): EmulationUiState {
    return copy(
        dpadOffset = profile.dpadOffset,
        lstickOffset = profile.lstickOffset,
        rstickOffset = profile.rstickOffset,
        actionOffset = profile.actionOffset,
        lbtnOffset = profile.lbtnOffset,
        rbtnOffset = profile.rbtnOffset,
        centerOffset = profile.centerOffset,
        stickScale = profile.stickScale,
        controlLayouts = profile.controlLayouts
    )
}

// Applies a per-game touch profile before the first frame so the global layout is
// never shown while the game is still starting.
internal fun EmulationUiState.withTouchControlsProfile(
    layout: TouchControlsLayoutProfile?,
    customControls: CustomTouchControlLibrary?
): EmulationUiState {
    val withLayout = layout?.let { withTouchControlsLayout(it) } ?: this
    return customControls?.let { withLayout.copy(customTouchControls = it) } ?: withLayout
}
