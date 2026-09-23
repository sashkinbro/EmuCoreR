package com.sbro.emucorer.ui.onboarding

import android.app.Activity
import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.sbro.emucorer.core.BiosValidator
import com.sbro.emucorer.core.EmulatorBridge
import com.sbro.emucorer.core.EmulatorDataLocation
import com.sbro.emucorer.core.EmulatorStorage
import com.sbro.emucorer.core.GpuHardwareProfiles
import com.sbro.emucorer.core.NativeApp
import com.sbro.emucorer.core.ProProductOffer
import com.sbro.emucorer.core.ProPurchaseManager
import com.sbro.emucorer.core.ProPurchaseTier
import com.sbro.emucorer.core.SetupValidator
import com.sbro.emucorer.core.StorageAccess
import com.sbro.emucorer.data.AppPreferences
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class OnboardingUiState(
    val biosPath: String? = null,
    val gamePath: String? = null,
    val gamePaths: List<String> = emptyList(),
    val emulatorDataPath: String? = null,
    val sdCardDataPath: String? = null,
    val biosValid: Boolean = false,
    val gamePathValid: Boolean = false,
    val canContinue: Boolean = false,
    val currentPage: Int = 0,
    val totalPages: Int = 5,
    val isProUnlocked: Boolean = false,
    val proPrice: String? = null,
    val proProducts: List<ProProductOffer> = emptyList(),
    val ownedProProductIds: Set<String> = emptySet(),
    val isProPurchaseStatusVerified: Boolean = false,
    val isProProductLoading: Boolean = false,
    val isProProductAvailable: Boolean = false,
    val isProPurchaseInProgress: Boolean = false,
    val proPurchaseMessageResId: Int? = null
)

class OnboardingViewModel(application: Application) : AndroidViewModel(application) {

    private val preferences = AppPreferences(application)
    private val proPurchaseManager = ProPurchaseManager.getInstance(application)
    private val _uiState = MutableStateFlow(OnboardingUiState())
    val uiState: StateFlow<OnboardingUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            launch {
                proPurchaseManager.state.collect { proState ->
                    updateState(
                        isProUnlocked = proState.isProUnlocked,
                        proPrice = proState.productPrice,
                        proProducts = proState.products,
                        ownedProProductIds = proState.ownedProductIds,
                        isProPurchaseStatusVerified = proState.isPurchaseStatusVerified,
                        isProProductLoading = proState.isProductLoading,
                        isProProductAvailable = proState.isProductAvailable,
                        isProPurchaseInProgress = proState.isPurchaseInProgress,
                        proPurchaseMessageResId = proState.messageResId
                    )
                }
            }
            launch {
                preferences.biosPath.distinctUntilChanged().collect { path ->
                    val biosValid = withContext(Dispatchers.IO) {
                        BiosValidator.hasUsableBiosFiles(getApplication(), path)
                    }
                    updateState(
                        biosPath = path,
                        biosValid = biosValid
                    )
                }
            }
            launch {
                preferences.gamePaths.distinctUntilChanged().collect { paths ->
                    val gamePathValid = withContext(Dispatchers.IO) {
                        SetupValidator.hasCoreReadableGameFile(getApplication(), paths)
                    }
                    updateState(
                        gamePath = paths.firstOrNull(),
                        gamePaths = paths,
                        gamePathValid = gamePathValid
                    )
                }
            }
            launch {
                preferences.emulatorDataPath.collect { path ->
                    updateState(emulatorDataPath = path)
                }
            }
            refreshEmulatorDataLocations()
        }
    }

    fun purchasePro(
        activity: Activity,
        tier: ProPurchaseTier = ProPurchaseTier.BASE
    ) {
        proPurchaseManager.purchase(activity, tier)
    }

    fun clearProPurchaseMessage() {
        proPurchaseManager.clearMessage()
    }

    fun setBiosPath(uri: Uri) {
        val application = getApplication<Application>()
        viewModelScope.launch(Dispatchers.IO) {
            val previousPath = preferences.biosPath.first()
            StorageAccess.takePersistableReadPermission(application, uri)
            preferences.setBiosPath(uri.toString())
            if (previousPath != uri.toString()) {
                StorageAccess.releasePersistedPermission(application, previousPath)
            }
            val audioSettings = preferences.settingsSnapshot.first()
            EmulatorBridge.applyRuntimeConfig(
                biosPath = uri.toString(),
                emulatorDataPath = _uiState.value.emulatorDataPath,
                renderer = audioSettings.renderer,
                gpuHardwareProfile = GpuHardwareProfiles.detectHardwareProfile(),
                audioVolume = audioSettings.audioVolume,
                audioFastForwardVolume = audioSettings.audioFastForwardVolume,
                audioMuted = audioSettings.audioMuted,
                audioInterpolation = audioSettings.audioInterpolation,
                audioSyncMode = audioSettings.audioSyncMode,
                audioLightweightSpu2 = audioSettings.audioLightweightSpu2,
                audioBackend = audioSettings.audioBackend,
                audioBufferMs = audioSettings.audioBufferMs,
                audioOutputLatencyMs = audioSettings.audioOutputLatencyMs,
                audioMinimalOutputLatency = audioSettings.audioMinimalOutputLatency,
                enableFastmem = audioSettings.enableFastmem,
                deinterlaceMode = audioSettings.deinterlaceMode,
                dithering = audioSettings.dithering,
                upscaleMultiplier = EmulatorBridge.getSetting("EmuCoreR", "UpscaleMultiplier", "float")?.toFloatOrNull()
                    ?: EmulatorBridge.getSetting("EmuCoreR", "UpscaleMultiplier", "int")?.toIntOrNull()?.toFloat()
                    ?: 1f
            )
        }
    }

    fun setGamePath(uri: Uri) {
        val application = getApplication<Application>()
        viewModelScope.launch(Dispatchers.IO) {
            StorageAccess.takePersistableReadPermission(application, uri)
            val rawPath = uri.toString()
            // Persist the user's valid SAF selection immediately. Game discovery runs in
            // the gamePaths collector on Dispatchers.IO and must not make the selection vanish.
            preferences.addGamePath(rawPath)
        }
    }

    fun removeGamePath(path: String) {
        viewModelScope.launch(Dispatchers.IO) {
            preferences.removeGamePath(path)
            StorageAccess.releasePersistedPermission(getApplication(), path)
        }
    }

    fun setEmulatorDataLocation(location: EmulatorDataLocation) {
        val application = getApplication<Application>()
        viewModelScope.launch(Dispatchers.IO) {
            val preparedRoot = EmulatorStorage.prepareStandardDataRoot(application, location)
            if (preparedRoot == null) {
                withContext(Dispatchers.Main) {
                    android.widget.Toast.makeText(
                        application,
                        com.sbro.emucorer.R.string.emulator_data_location_error,
                        android.widget.Toast.LENGTH_LONG
                    ).show()
                }
                refreshEmulatorDataLocations()
                return@launch
            }
            preferences.setEmulatorDataPath(preparedRoot.preferencePath)
            NativeApp.reloadDataRoot(preparedRoot.preferencePath ?: "")
        }
    }

    fun refreshEmulatorDataLocations() {
        viewModelScope.launch(Dispatchers.IO) {
            val sdCardDataPath = EmulatorStorage.sdCardRoot(getApplication())?.absolutePath
            withContext(Dispatchers.Main) {
                updateState(sdCardDataPath = sdCardDataPath)
            }
        }
    }

    fun setCurrentPage(page: Int) {
        val currentState = _uiState.value
        _uiState.value = currentState.copy(currentPage = page.coerceIn(0, currentState.totalPages - 1))
    }

    fun completeOnboarding(onFinished: () -> Unit) {
        if (!_uiState.value.canContinue) return
        viewModelScope.launch {
            preferences.setOnboardingCompleted(true)
            onFinished()
        }
    }

    private fun updateState(
        biosPath: String? = _uiState.value.biosPath,
        gamePath: String? = _uiState.value.gamePath,
        gamePaths: List<String> = _uiState.value.gamePaths,
        emulatorDataPath: String? = _uiState.value.emulatorDataPath,
        sdCardDataPath: String? = _uiState.value.sdCardDataPath,
        biosValid: Boolean = _uiState.value.biosValid,
        gamePathValid: Boolean = _uiState.value.gamePathValid,
        currentPage: Int = _uiState.value.currentPage,
        isProUnlocked: Boolean = _uiState.value.isProUnlocked,
        proPrice: String? = _uiState.value.proPrice,
        proProducts: List<ProProductOffer> = _uiState.value.proProducts,
        ownedProProductIds: Set<String> = _uiState.value.ownedProProductIds,
        isProPurchaseStatusVerified: Boolean = _uiState.value.isProPurchaseStatusVerified,
        isProProductLoading: Boolean = _uiState.value.isProProductLoading,
        isProProductAvailable: Boolean = _uiState.value.isProProductAvailable,
        isProPurchaseInProgress: Boolean = _uiState.value.isProPurchaseInProgress,
        proPurchaseMessageResId: Int? = _uiState.value.proPurchaseMessageResId
    ) {
        _uiState.value = OnboardingUiState(
            biosPath = biosPath,
            gamePath = gamePath,
            gamePaths = gamePaths,
            emulatorDataPath = emulatorDataPath,
            sdCardDataPath = sdCardDataPath,
            biosValid = biosValid,
            gamePathValid = gamePathValid,
            canContinue = biosValid && gamePathValid,
            currentPage = currentPage.coerceIn(0, 4),
            totalPages = 5,
            isProUnlocked = isProUnlocked,
            proPrice = proPrice,
            proProducts = proProducts,
            ownedProProductIds = ownedProProductIds,
            isProPurchaseStatusVerified = isProPurchaseStatusVerified,
            isProProductLoading = isProProductLoading,
            isProProductAvailable = isProProductAvailable,
            isProPurchaseInProgress = isProPurchaseInProgress,
            proPurchaseMessageResId = proPurchaseMessageResId
        )
    }
}
