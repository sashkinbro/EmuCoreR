
package com.sbro.emucorer.ui.emulation

import com.sbro.emucorer.core.RuntimeFailure

internal fun EmulationUiState.shouldAutoSaveOnExit(gamePath: String?, failure: RuntimeFailure?): Boolean =
    autoSaveOnExit && gamePath != null && failure == null

internal fun EmulationUiState.withRuntimeFailure(failure: RuntimeFailure?): EmulationUiState =
    if (failure == null) this else copy(
        runtimeFailure = failure,
        isRunning = false,
        isStarting = false,
        isPaused = false,
        showMenu = false,
        isActionInProgress = false,
        actionLabel = null,
        statusMessage = null,
        toastMessage = null,
        fps = "0",
        performanceOverlayText = "",
        transportMode = EmulationTransportMode.None
    )
