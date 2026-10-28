package com.sbro.emucorer.ui.common

import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.EaseIn
import androidx.compose.animation.core.EaseOut
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut

internal fun gpuDriverEnterTransition(): EnterTransition =
    fadeIn(tween(260, delayMillis = 70, easing = EaseOut)) +
        scaleIn(initialScale = 0.96f, animationSpec = tween(260, delayMillis = 70, easing = EaseOut))

internal fun gpuDriverExitTransition(): ExitTransition =
    fadeOut(tween(110, easing = EaseIn)) +
        scaleOut(targetScale = 1f, animationSpec = tween(110, easing = EaseIn))
