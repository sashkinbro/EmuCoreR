package com.sbro.emucorer.ui.emulation

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.core.tween
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier

@Composable
internal fun EmulationMenuVisibility(
    uiState: EmulationUiState,
    showControlsEditor: Boolean,
    modifier: Modifier = Modifier,
    content: @Composable (EmulationUiState) -> Unit
) {
    val visible = uiState.showMenu && !showControlsEditor
    var lastVisibleState by remember { mutableStateOf(uiState) }
    val menuState = if (visible) uiState else lastVisibleState
    SideEffect {
        if (visible) lastVisibleState = uiState
    }
    // Shutdown clears the live game metadata while AnimatedVisibility is still drawing its exit.
    AnimatedVisibility(
        visible = visible,
        enter = slideInHorizontally(initialOffsetX = { it }) + fadeIn(tween(300)),
        exit = slideOutHorizontally(targetOffsetX = { it }) + fadeOut(tween(250)),
        modifier = modifier
    ) {
        content(menuState)
    }
}
