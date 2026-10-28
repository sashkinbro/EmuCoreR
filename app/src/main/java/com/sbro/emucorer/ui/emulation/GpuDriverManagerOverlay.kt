package com.sbro.emucorer.ui.emulation

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.zIndex
import com.sbro.emucorer.ui.common.gpuDriverEnterTransition
import com.sbro.emucorer.ui.common.gpuDriverExitTransition

@Composable
internal fun GpuDriverManagerOverlay(
    visible: Boolean,
    onBack: () -> Unit,
    content: @Composable () -> Unit
) {
    BackHandler(enabled = visible, onBack = onBack)
    AnimatedVisibility(
        visible = visible,
        modifier = Modifier.fillMaxSize().zIndex(100f),
        enter = gpuDriverEnterTransition(),
        exit = gpuDriverExitTransition()
    ) {
        Surface(
            modifier = Modifier.fillMaxSize().clickable(
                interactionSource = remember { MutableInteractionSource() },
                indication = null,
                onClick = {}
            ),
            color = MaterialTheme.colorScheme.background,
            content = content
        )
    }
}
