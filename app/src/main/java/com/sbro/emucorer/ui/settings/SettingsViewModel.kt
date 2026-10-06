package com.sbro.emucorer.ui.settings

import android.app.Activity
import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.sbro.emucorer.BuildConfig
import com.sbro.emucorer.core.AndroidTouchHaptics
import com.sbro.emucorer.core.AudioDefaults
import com.sbro.emucorer.core.AppUpdateRelease
import com.sbro.emucorer.core.AppUpdateRepository
import com.sbro.emucorer.core.BiosValidator
import com.sbro.emucorer.core.DocumentPathResolver
import com.sbro.emucorer.core.EmulatorBridge
import com.sbro.emucorer.core.EmulatorDataLocation
import com.sbro.emucorer.core.EmulatorStorage
import com.sbro.emucorer.core.GpuHardwareProfiles
import com.sbro.emucorer.core.RendererDefaults
import com.sbro.emucorer.core.GamepadManager
import com.sbro.emucorer.core.ProProductOffer
import com.sbro.emucorer.core.ProPurchaseManager
import com.sbro.emucorer.core.ProPurchaseState
import com.sbro.emucorer.core.ProPurchaseTier
import com.sbro.emucorer.core.NativeApp
import com.sbro.emucorer.core.SetupValidator
import com.sbro.emucorer.core.StorageAccess
import com.sbro.emucorer.core.TvInterfaceMode
import com.sbro.emucorer.core.normalizeUpscale
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.data.DisplayCrop
import com.sbro.emucorer.data.AppFontChoice
import com.sbro.emucorer.data.HomeBackgroundRepository
import com.sbro.emucorer.data.HomeBackgroundPreset
import com.sbro.emucorer.data.HomeBackgroundType
import com.sbro.emucorer.data.EmulationSideArtwork
import com.sbro.emucorer.data.EmulationSideArtworkRepository
import com.sbro.emucorer.data.RetroArchShaderPreset
import com.sbro.emucorer.data.RetroArchShaderRepository
import com.sbro.emucorer.data.PatchDatabaseDownloader
import com.sbro.emucorer.data.PatchDatabaseInstallProgress
import com.sbro.emucorer.data.PatchDatabaseInstallStage
import com.sbro.emucorer.data.ShaderPackInstallProgress
import com.sbro.emucorer.data.ShaderPackInstallStage
import com.sbro.emucorer.data.TouchControlVisualStyle
import com.sbro.emucorer.data.TouchControlPressEffect
import com.sbro.emucorer.data.GameMenuLayoutStyle
import com.sbro.emucorer.data.DrawerVisualStyle
import com.sbro.emucorer.data.DrawerItemId
import com.sbro.emucorer.data.GameMenuTabId
import com.sbro.emucorer.data.GameMenuSectionId
import com.sbro.emucorer.data.DefaultGameMenuTabOrder
import com.sbro.emucorer.data.DefaultGameMenuSectionOrder
import com.sbro.emucorer.data.AppPreferences.Companion.FPS_OVERLAY_MODE_DETAILED
import com.sbro.emucorer.data.CoverArtRepository
import com.sbro.emucorer.data.CoverCacheClearResult
import com.sbro.emucorer.data.CustomFontRepository
import com.sbro.emucorer.data.CustomThemeConfig
import com.sbro.emucorer.data.CustomThemeLibrary
import com.sbro.emucorer.data.CustomTouchControlLibrary
import com.sbro.emucorer.data.SettingsSnapshot
import com.sbro.emucorer.data.PerformanceOverlayMetrics
import com.sbro.emucorer.ui.common.clearCoverImageMemoryCache
import com.sbro.emucorer.ui.theme.ThemeMode
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.withContext
import kotlin.time.Duration.Companion.milliseconds
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

data class SettingsUiState(
    val isLoaded: Boolean = false,
    val showMediatekCompatibilityNotice: Boolean = false,
    val themeMode: ThemeMode = ThemeMode.SYSTEM,
    val customTheme: CustomThemeConfig = CustomThemeConfig.Default,
    val customThemeLibrary: CustomThemeLibrary = CustomThemeLibrary.Empty,
    val customTouchControls: CustomTouchControlLibrary = CustomTouchControlLibrary.Empty,
    val appFontChoice: AppFontChoice = AppFontChoice.SYSTEM,
    val appFontScale: Float = AppPreferences.DEFAULT_APP_FONT_SCALE,
    val customFontName: String? = null,
    val customFontRevision: Int = 0,
    val homeGridScale: Float = AppPreferences.DEFAULT_HOME_GRID_SCALE,
    val homeBackgroundType: HomeBackgroundType = HomeBackgroundType.NONE,
    val homeBackgroundPreset: HomeBackgroundPreset = HomeBackgroundPreset.OLYMPUS,
    val homeBackgroundRevision: Int = 0,
    val homeBackgroundDim: Int = AppPreferences.DEFAULT_HOME_BACKGROUND_DIM,
    val emulationSideArtwork: EmulationSideArtwork = EmulationSideArtwork.NONE,
    val emulationSideArtworkRevision: Int = 0,
    val emulationSideArtworkDim: Int = AppPreferences.DEFAULT_EMULATION_SIDE_ARTWORK_DIM,
    val isSideArtworkImporting: Boolean = false,
    val shaderChainEnabled: Boolean = false,
    val shaderChainPreset: String = "",
    val shaderPresets: List<RetroArchShaderPreset> = emptyList(),
    val isShaderPackInstalled: Boolean = false,
    val isShaderPackBusy: Boolean = false,
    val shaderPackProgress: ShaderPackInstallProgress? = null,
    val shaderPackMessageResId: Int? = null,
    val patchDatabaseUseOfficial: Boolean = true,
    val patchDatabaseCustomUrl: String? = null,
    val installedPatchCount: Int = 0,
    val isPatchDatabaseBusy: Boolean = false,
    val patchDatabaseProgress: PatchDatabaseInstallProgress? = null,
    val patchDatabaseMessageResId: Int? = null,
    val patchDatabaseMessageCount: Int? = null,
    val touchControlVisualStyle: TouchControlVisualStyle = TouchControlVisualStyle.CLASSIC,
    val touchControlPressEffect: TouchControlPressEffect = TouchControlPressEffect.GROW,
    val gameMenuLayoutStyle: GameMenuLayoutStyle = GameMenuLayoutStyle.SIDEBAR,
    val drawerVisualStyle: DrawerVisualStyle = DrawerVisualStyle.CLASSIC,
    val hiddenDrawerItems: Set<DrawerItemId> = emptySet(),
    val gameMenuTabOrder: List<GameMenuTabId> = DefaultGameMenuTabOrder,
    val hiddenGameMenuTabs: Set<GameMenuTabId> = emptySet(),
    val gameMenuSectionOrder: List<GameMenuSectionId> = DefaultGameMenuSectionOrder,
    val hiddenGameMenuSections: Set<GameMenuSectionId> = emptySet(),
    val isBackgroundImporting: Boolean = false,
    val customizationMessageResId: Int? = null,
    val proState: ProPurchaseState = ProPurchaseState(),
    val languageTag: String? = null,
    val tvInterfaceMode: TvInterfaceMode = TvInterfaceMode.AUTO,
    val renderer: Int = RendererDefaults.defaultForHardware(),
    val upscaleMultiplier: Float = 1f,
    val aspectRatio: Int = 1,
    val localMultiplayerMode: Int = AppPreferences.LOCAL_MULTIPLAYER_OFF,
    val displayCrop: DisplayCrop = DisplayCrop.None,
    val audioVolume: Int = AudioDefaults.VOLUME_DEFAULT,
    val audioMuted: Boolean = false,
    val audioOutputLatencyMs: Int = AudioDefaults.OUTPUT_LATENCY_MS_DEFAULT,
    val audioMinimalOutputLatency: Boolean = AudioDefaults.MINIMAL_OUTPUT_LATENCY_DEFAULT,
    val padVibration: Boolean = true,
    val padVibrationStrength: Int = AppPreferences.DEFAULT_PAD_VIBRATION_STRENGTH,
    val padVibrationFallback: Boolean = true,
    val showFps: Boolean = true,
    val fpsOverlayMode: Int = FPS_OVERLAY_MODE_DETAILED,
    val fpsOverlayCorner: Int = AppPreferences.FPS_OVERLAY_CORNER_TOP_RIGHT,
    val fpsOverlayScale: Int = AppPreferences.DEFAULT_FPS_OVERLAY_SCALE,
    val fpsOverlayMetrics: Int = PerformanceOverlayMetrics.DEFAULT,
    val confirmSaveLoadActions: Boolean = true,
    val backButtonExitsGame: Boolean = false,
    val compactControls: Boolean = true,
    val keepScreenOn: Boolean = true,
    val respectDisplayCutout: Boolean = false,
    val showRecentGames: Boolean = true,
    val showHomeSearch: Boolean = false,
    val showDebugOptions: Boolean = false,
    val preferEnglishGameTitles: Boolean = false,
    val biosPath: String? = null,
    val gamePath: String? = null,
    val gamePaths: List<String> = emptyList(),
    val emulatorDataPath: String? = null,
    val sdCardDataPath: String? = null,
    val coverDownloadBaseUrl: String? = null,
    val coverArtStyle: Int = AppPreferences.COVER_ART_STYLE_DEFAULT,
    val biosValid: Boolean = false,
    val setupComplete: Boolean = false,
    val appVersion: String = BuildConfig.VERSION_NAME,
    val coreName: String = "unknown",
    val coreVersion: String = "unknown",
    val enableFastBoot: Boolean = true,
    val enableCheats: Boolean = false,
    val frameSkip: Int = 0,
    val textureFiltering: Int = 0,
    val enableWidescreenPatches: Boolean = false,
    // Overlay
    val overlayScale: Int = 100,
    val overlayOpacity: Int = AppPreferences.DEFAULT_OVERLAY_OPACITY,
    val overlayShow: Boolean = true,
    val racingMode: Boolean = false,
    val stickyButtons: Set<String> = emptySet(),
    val touchscreenRightStick: Boolean = AppPreferences.DEFAULT_TOUCHSCREEN_RIGHT_STICK,
    val touchscreenRightStickSensitivity: Int = AppPreferences.DEFAULT_TOUCHSCREEN_RIGHT_STICK_SENSITIVITY,
    val touchHaptics: Boolean = false,
    val touchHapticsPreset: Int = AppPreferences.DEFAULT_TOUCH_HAPTICS_PRESET,
    val touchHapticsStrength: Int = AppPreferences.DEFAULT_TOUCH_HAPTICS_STRENGTH,
    val gyroMode: Int = AppPreferences.GYRO_MODE_OFF,
    val gyroSensitivity: Int = AppPreferences.DEFAULT_GYRO_SENSITIVITY,
    val gyroSmoothing: Int = AppPreferences.DEFAULT_GYRO_SMOOTHING,
    val gyroInvertX: Boolean = false,
    val gyroInvertY: Boolean = false,
    val leftStickSensitivity: Int = AppPreferences.DEFAULT_STICK_SENSITIVITY,
    val rightStickSensitivity: Int = AppPreferences.DEFAULT_STICK_SENSITIVITY,
    val invertLeftStick: Boolean = false,
    val invertRightStick: Boolean = false,
    val invertLeftStickHorizontal: Boolean = false,
    val invertRightStickHorizontal: Boolean = false,
    // Gamepad
    val enableAutoGamepad: Boolean = true,
    val hideOverlayOnGamepad: Boolean = true,
    val gamepadStickDeadzone: Int = AppPreferences.DEFAULT_GAMEPAD_STICK_DEADZONE,
    val gamepadLeftStickSensitivity: Int = AppPreferences.DEFAULT_GAMEPAD_STICK_SENSITIVITY,
    val gamepadRightStickSensitivity: Int = AppPreferences.DEFAULT_GAMEPAD_STICK_SENSITIVITY,
    val gamepadRightStickUpToR2: Boolean = false,
    val gamepadRightStickDownToL2: Boolean = false,
    val gamepadButtonHaptics: Boolean = false,
    val gamepadBindings: Map<String, Int> = emptyMap(),
    val gamepadBindingsByPad: Map<Int, Map<String, Int>> = emptyMap(),
    val appUpdate: AppUpdateUiState = AppUpdateUiState(),
    val frameLimitEnabled: Boolean = true,
    val targetFps: Int = 0,
    val enableIcacheEmulation: Boolean = false,
    val enableCddaAudio: Boolean = true,
    val multitapMode: Int = 0,
    val cdReadAhead: Int = 0
) {
    // Derived views over proState. They keep the huge SettingsUiState data class below the
    // dex limit of 255 method parameters for the generated copy$default method.
    val isProUnlocked: Boolean get() = proState.isProUnlocked
    val proPrice: String? get() = proState.productPrice
    val proProducts: List<ProProductOffer> get() = proState.products
    val ownedProProductIds: Set<String> get() = proState.ownedProductIds
    val isProPurchaseStatusVerified: Boolean get() = proState.isPurchaseStatusVerified
    val isProProductLoading: Boolean get() = proState.isProductLoading
    val isProProductAvailable: Boolean get() = proState.isProductAvailable
    val isProPurchaseInProgress: Boolean get() = proState.isPurchaseInProgress
    val proPurchaseMessageResId: Int? get() = proState.messageResId
}

data class AppUpdateUiState(
    val releaseHistory: List<AppUpdateRelease> = emptyList(),
    val historyLoading: Boolean = false,
    val historyErrorMessage: String? = null
)

class SettingsViewModel(application: Application) : AndroidViewModel(application) {

    private val preferences = AppPreferences(application)
    private val customFontRepository = CustomFontRepository(application)
    private val homeBackgroundRepository = HomeBackgroundRepository(application)
    private val emulationSideArtworkRepository = EmulationSideArtworkRepository(application)
    private val retroArchShaderRepository = RetroArchShaderRepository(application)
    private val patchDatabaseDownloader = PatchDatabaseDownloader(application)
    private val appUpdateRepository = AppUpdateRepository(application)
    private val proPurchaseManager = ProPurchaseManager.getInstance(application)
    private val _uiState = MutableStateFlow(SettingsUiState())
    val uiState: StateFlow<SettingsUiState> = _uiState.asStateFlow()
    val floatingQuickActionsEnabled: StateFlow<Boolean> = preferences.floatingQuickActionsEnabled
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), false)
    val orientationLock: StateFlow<Int> = preferences.orientationLock
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), AppPreferences.ORIENTATION_LOCK_AUTO)
    val emulationAllowsBothOrientations: StateFlow<Boolean> = preferences.emulationAllowsBothOrientations
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), false)
    private var mediatekCompatibilityNoticeChecked = false

    init {
        initializeAboutInfo()
        refreshShaderPresets()
        refreshPatchDatabaseState()
        viewModelScope.launch {
            preferences.settingsSnapshot.collect { snapshot ->
                applySettingsSnapshot(snapshot)
            }
        }
        viewModelScope.launch {
            preferences.biosPath.distinctUntilChanged().collect { path ->
                val biosValid = withContext(Dispatchers.IO) {
                    BiosValidator.hasUsableBiosFiles(getApplication(), path)
                }
                _uiState.value = _uiState.value.copy(biosValid = biosValid)
            }
        }
        refreshEmulatorDataLocations()
        viewModelScope.launch {
            proPurchaseManager.state.collect { proState ->
                _uiState.value = _uiState.value.copy(proState = proState)
            }
        }
    }

    private fun initializeAboutInfo() {
        val application = getApplication<Application>()
        val appVersion = runCatching {
            application.packageManager.getPackageInfo(application.packageName, 0).versionName
        }.getOrNull()?.takeIf { it.isNotBlank() } ?: BuildConfig.VERSION_NAME
        _uiState.value = _uiState.value.copy(
            appVersion = appVersion,
            coreName = CORE_NAME,
            coreVersion = CORE_VERSION
        )
    }

    private fun applySettingsSnapshot(snapshot: SettingsSnapshot) {
        _uiState.value = _uiState.value.copy(
            isLoaded = true,
            themeMode = snapshot.themeMode,
            customTheme = snapshot.customTheme,
            customThemeLibrary = snapshot.customThemeLibrary,
            customTouchControls = snapshot.customTouchControls,
            appFontChoice = snapshot.appFontChoice,
            appFontScale = snapshot.appFontScale,
            customFontName = snapshot.customFontName,
            customFontRevision = snapshot.customFontRevision,
            homeGridScale = snapshot.homeGridScale,
            homeBackgroundType = snapshot.homeBackgroundType,
            homeBackgroundPreset = snapshot.homeBackgroundPreset,
            homeBackgroundRevision = snapshot.homeBackgroundRevision,
            homeBackgroundDim = snapshot.homeBackgroundDim,
            emulationSideArtwork = snapshot.emulationSideArtwork,
            emulationSideArtworkRevision = snapshot.emulationSideArtworkRevision,
            emulationSideArtworkDim = snapshot.emulationSideArtworkDim,
            shaderChainEnabled = snapshot.shaderChainEnabled,
            shaderChainPreset = snapshot.shaderChainPreset,
            touchControlVisualStyle = snapshot.touchControlVisualStyle,
            touchControlPressEffect = snapshot.touchControlPressEffect,
            gameMenuLayoutStyle = snapshot.gameMenuLayoutStyle,
            drawerVisualStyle = snapshot.drawerVisualStyle,
            hiddenDrawerItems = snapshot.hiddenDrawerItems,
            gameMenuTabOrder = snapshot.gameMenuTabOrder,
            hiddenGameMenuTabs = snapshot.hiddenGameMenuTabs,
            gameMenuSectionOrder = snapshot.gameMenuSectionOrder,
            hiddenGameMenuSections = snapshot.hiddenGameMenuSections,
            languageTag = snapshot.languageTag,
            tvInterfaceMode = snapshot.tvInterfaceMode,
            renderer = snapshot.renderer,
            upscaleMultiplier = snapshot.upscaleMultiplier,
            aspectRatio = snapshot.aspectRatio,
            localMultiplayerMode = snapshot.localMultiplayerMode,
            displayCrop = snapshot.displayCrop,
            audioVolume = snapshot.audioVolume,
            audioMuted = snapshot.audioMuted,
            audioOutputLatencyMs = snapshot.audioOutputLatencyMs,
            audioMinimalOutputLatency = snapshot.audioMinimalOutputLatency,
            padVibration = snapshot.padVibration,
            padVibrationStrength = snapshot.padVibrationStrength,
            padVibrationFallback = snapshot.padVibrationFallback,
            showFps = snapshot.showFps,
            fpsOverlayMode = snapshot.fpsOverlayMode,
            fpsOverlayCorner = snapshot.fpsOverlayCorner,
            fpsOverlayScale = snapshot.fpsOverlayScale,
            fpsOverlayMetrics = snapshot.fpsOverlayMetrics,
            confirmSaveLoadActions = snapshot.confirmSaveLoadActions,
            backButtonExitsGame = snapshot.backButtonExitsGame,
            compactControls = snapshot.compactControls,
            keepScreenOn = snapshot.keepScreenOn,
            respectDisplayCutout = snapshot.respectDisplayCutout,
            showRecentGames = snapshot.showRecentGames,
            showHomeSearch = snapshot.showHomeSearch,
            showDebugOptions = snapshot.showDebugOptions,
            preferEnglishGameTitles = snapshot.preferEnglishGameTitles,
            biosPath = snapshot.biosPath,
            gamePath = snapshot.gamePath,
            gamePaths = snapshot.gamePaths,
            emulatorDataPath = snapshot.emulatorDataPath,
            coverDownloadBaseUrl = snapshot.coverDownloadBaseUrl,
            coverArtStyle = snapshot.coverArtStyle,
            setupComplete = snapshot.setupComplete,
            enableFastBoot = snapshot.enableFastBoot,
            enableCheats = snapshot.enableCheats,
            frameSkip = snapshot.frameSkip,
            textureFiltering = snapshot.textureFiltering,
            enableWidescreenPatches = snapshot.enableWidescreenPatches,
            patchDatabaseUseOfficial = snapshot.patchDatabaseUseOfficial,
            patchDatabaseCustomUrl = snapshot.patchDatabaseCustomUrl,
            overlayScale = snapshot.overlayScale,
            overlayOpacity = snapshot.overlayOpacity,
            overlayShow = snapshot.overlayShow,
            racingMode = snapshot.racingMode,
            stickyButtons = snapshot.stickyButtons,
            touchscreenRightStick = snapshot.touchscreenRightStick,
            touchscreenRightStickSensitivity = snapshot.touchscreenRightStickSensitivity,
            touchHaptics = snapshot.touchHaptics,
            touchHapticsPreset = snapshot.touchHapticsPreset,
            touchHapticsStrength = snapshot.touchHapticsStrength,
            gyroMode = snapshot.gyroMode,
            gyroSensitivity = snapshot.gyroSensitivity,
            gyroSmoothing = snapshot.gyroSmoothing,
            gyroInvertX = snapshot.gyroInvertX,
            gyroInvertY = snapshot.gyroInvertY,
            leftStickSensitivity = snapshot.leftStickSensitivity,
            rightStickSensitivity = snapshot.rightStickSensitivity,
            invertLeftStick = snapshot.invertLeftStick,
            invertRightStick = snapshot.invertRightStick,
            invertLeftStickHorizontal = snapshot.invertLeftStickHorizontal,
            invertRightStickHorizontal = snapshot.invertRightStickHorizontal,
            enableAutoGamepad = snapshot.enableAutoGamepad,
            hideOverlayOnGamepad = snapshot.hideOverlayOnGamepad,
            gamepadStickDeadzone = snapshot.gamepadStickDeadzone,
            gamepadLeftStickSensitivity = snapshot.gamepadLeftStickSensitivity,
            gamepadRightStickSensitivity = snapshot.gamepadRightStickSensitivity,
            gamepadRightStickUpToR2 = snapshot.gamepadRightStickUpToR2,
            gamepadRightStickDownToL2 = snapshot.gamepadRightStickDownToL2,
            gamepadButtonHaptics = snapshot.gamepadButtonHaptics,
            gamepadBindings = snapshot.gamepadBindings,
            gamepadBindingsByPad = snapshot.gamepadBindingsByPad,
            frameLimitEnabled = snapshot.frameLimitEnabled,
            targetFps = snapshot.targetFps,
            enableIcacheEmulation = snapshot.enableIcacheEmulation,
            enableCddaAudio = snapshot.enableCddaAudio,
            multitapMode = snapshot.multitapMode,
            cdReadAhead = snapshot.cdReadAhead
        )
    }

    fun checkMediatekCompatibilityNotice() {
        if (mediatekCompatibilityNoticeChecked) return
        mediatekCompatibilityNoticeChecked = true
        if (!GpuHardwareProfiles.isMediaTekHardware()) return

        viewModelScope.launch {
            if (!preferences.mediatekSettingsNoticeShown.first()) {
                _uiState.value = _uiState.value.copy(showMediatekCompatibilityNotice = true)
            }
        }
    }

    fun dismissMediatekCompatibilityNotice() {
        _uiState.value = _uiState.value.copy(showMediatekCompatibilityNotice = false)
        viewModelScope.launch {
            preferences.markMediatekSettingsNoticeShown()
        }
    }

    fun purchasePro(
        activity: Activity,
        tier: ProPurchaseTier = ProPurchaseTier.BASE
    ) {
        proPurchaseManager.purchase(activity, tier)
    }

    fun restoreProPurchases() { proPurchaseManager.restorePurchases(showMessage = true) }

    fun clearProPurchaseMessage() { proPurchaseManager.clearMessage() }

    fun setThemeMode(mode: ThemeMode) = viewModelScope.launch {
        preferences.setThemeMode(mode)
        if (mode == ThemeMode.NEON) {
            preferences.setTouchControlVisualStyle(TouchControlVisualStyle.MODERN)
        }
    }
    fun saveCustomTheme(config: CustomThemeConfig, activate: Boolean) = viewModelScope.launch {
        if (!_uiState.value.isProUnlocked) return@launch
        if (activate) preferences.applyCustomTheme(config) else preferences.setCustomTheme(config)
    }
    fun saveCustomThemeLibrary(library: CustomThemeLibrary, activate: Boolean) {
        val current = _uiState.value
        if (!current.isProUnlocked) return

        val safeLibrary = library.sanitized()
        val activeConfig = safeLibrary.activeTheme()?.config
        val nextThemeMode = when {
            activate && activeConfig != null -> ThemeMode.CUSTOM
            current.themeMode == ThemeMode.CUSTOM && activeConfig == null -> ThemeMode.SYSTEM
            else -> current.themeMode
        }
        _uiState.value = current.copy(
            themeMode = nextThemeMode,
            customTheme = activeConfig ?: CustomThemeConfig.Default,
            customThemeLibrary = safeLibrary
        )
        viewModelScope.launch {
            preferences.setCustomThemeLibrary(safeLibrary, activate)
        }
    }

    fun saveCustomTouchControls(library: CustomTouchControlLibrary) {
        val current = _uiState.value
        if (!current.isProUnlocked) return

        val safeLibrary = library.sanitized()
        _uiState.value = current.copy(customTouchControls = safeLibrary)
        viewModelScope.launch {
            preferences.setCustomTouchControls(safeLibrary)
        }
    }
    fun setAppFontChoice(choice: AppFontChoice) = viewModelScope.launch {
        if (choice == AppFontChoice.CUSTOM && customFontRepository.installedFile() == null) return@launch
        preferences.setAppFontChoice(choice)
    }

    fun installCustomFont(uri: Uri) = viewModelScope.launch {
        _uiState.value = _uiState.value.copy(customizationMessageResId = null)
        val result = customFontRepository.install(uri)
        result.getOrNull()?.let { installed ->
            preferences.setCustomFontInstalled(installed.displayName)
        }
        _uiState.value = _uiState.value.copy(
            customizationMessageResId = if (result.isSuccess) {
                com.sbro.emucorer.R.string.settings_customization_custom_font_applied
            } else {
                com.sbro.emucorer.R.string.settings_customization_custom_font_failed
            }
        )
    }

    fun clearCustomFont() = viewModelScope.launch(Dispatchers.IO) {
        customFontRepository.clear()
        preferences.clearCustomFont()
        _uiState.value = _uiState.value.copy(
            customizationMessageResId = com.sbro.emucorer.R.string.settings_customization_custom_font_removed
        )
    }

    fun setAppFontScale(scale: Float) = viewModelScope.launch {
        preferences.setAppFontScale(scale)
    }

    fun setHomeGridScale(scale: Float) = viewModelScope.launch {
        preferences.setHomeGridScale(scale)
    }

    fun setHomeBackgroundDim(dim: Int) = viewModelScope.launch {
        preferences.setHomeBackgroundDim(dim)
    }

    fun setEmulationSideArtworkDim(dim: Int) = viewModelScope.launch {
        preferences.setEmulationSideArtworkDim(dim)
    }

    fun setTouchControlVisualStyle(style: TouchControlVisualStyle) = viewModelScope.launch {
        preferences.setTouchControlVisualStyle(style)
    }

    fun setGameMenuLayoutStyle(style: GameMenuLayoutStyle) = viewModelScope.launch {
        preferences.setGameMenuLayoutStyle(style)
    }

    fun setDrawerVisualStyle(style: DrawerVisualStyle) = viewModelScope.launch {
        preferences.setDrawerVisualStyle(style)
    }

    fun setDrawerItemVisible(item: DrawerItemId, visible: Boolean) = viewModelScope.launch {
        if (item.required) return@launch
        val hidden = _uiState.value.hiddenDrawerItems.toMutableSet()
        if (visible) hidden.remove(item) else hidden.add(item)
        preferences.setHiddenDrawerItems(hidden)
    }

    fun setGameMenuTabVisible(tab: GameMenuTabId, visible: Boolean) = viewModelScope.launch {
        if (tab == GameMenuTabId.SESSION) return@launch
        val hidden = _uiState.value.hiddenGameMenuTabs.toMutableSet()
        if (visible) hidden.remove(tab) else hidden.add(tab)
        preferences.setHiddenGameMenuTabs(hidden)
    }

    fun moveGameMenuTab(tab: GameMenuTabId, direction: Int) = viewModelScope.launch {
        val order = _uiState.value.gameMenuTabOrder.toMutableList()
        val from = order.indexOf(tab)
        val to = (from + direction).coerceIn(0, order.lastIndex)
        if (from >= 0 && from != to) {
            order.removeAt(from)
            order.add(to, tab)
            preferences.setGameMenuTabOrder(order)
        }
    }

    fun setGameMenuSectionVisible(section: GameMenuSectionId, visible: Boolean) = viewModelScope.launch {
        val hidden = _uiState.value.hiddenGameMenuSections.toMutableSet()
        if (visible) hidden.remove(section) else hidden.add(section)
        preferences.setHiddenGameMenuSections(hidden)
    }

    fun moveGameMenuSection(section: GameMenuSectionId, direction: Int) = viewModelScope.launch {
        val order = _uiState.value.gameMenuSectionOrder.toMutableList()
        val sameTab = order.filter { it.tab == section.tab }
        val fromWithinTab = sameTab.indexOf(section)
        val toWithinTab = (fromWithinTab + direction).coerceIn(0, sameTab.lastIndex)
        if (fromWithinTab < 0 || fromWithinTab == toWithinTab) return@launch

        val reorderedTab = sameTab.toMutableList().apply {
            removeAt(fromWithinTab)
            add(toWithinTab, section)
        }
        var nextIndex = 0
        val normalized = order.map { current ->
            if (current.tab == section.tab) reorderedTab[nextIndex++] else current
        }
        preferences.setGameMenuSectionOrder(normalized)
    }

    fun resetGameMenuCustomization() = viewModelScope.launch {
        preferences.setGameMenuLayoutStyle(GameMenuLayoutStyle.SIDEBAR)
        preferences.setGameMenuTabOrder(DefaultGameMenuTabOrder)
        preferences.setHiddenGameMenuTabs(emptySet())
        preferences.setGameMenuSectionOrder(DefaultGameMenuSectionOrder)
        preferences.setHiddenGameMenuSections(emptySet())
    }

    fun setTouchControlPressEffect(effect: TouchControlPressEffect) = viewModelScope.launch {
        preferences.setTouchControlPressEffect(effect)
    }

    fun installHomeBackground(uri: Uri) = viewModelScope.launch {
        _uiState.value = _uiState.value.copy(
            isBackgroundImporting = true,
            customizationMessageResId = null
        )
        val result = homeBackgroundRepository.install(uri)
        result.getOrNull()?.let { preferences.setHomeBackgroundType(it) }
        _uiState.value = _uiState.value.copy(
            isBackgroundImporting = false,
            customizationMessageResId = if (result.isSuccess) {
                com.sbro.emucorer.R.string.settings_customization_background_applied
            } else {
                com.sbro.emucorer.R.string.settings_customization_background_failed
            }
        )
    }

    fun setHomeBackgroundPreset(preset: HomeBackgroundPreset) = viewModelScope.launch {
        preferences.setHomeBackgroundPreset(preset)
    }

    fun clearHomeBackground() = viewModelScope.launch(Dispatchers.IO) {
        homeBackgroundRepository.clear()
        preferences.setHomeBackgroundType(HomeBackgroundType.NONE)
        _uiState.value = _uiState.value.copy(
            customizationMessageResId = com.sbro.emucorer.R.string.settings_customization_background_removed
        )
    }

    fun setEmulationSideArtwork(artwork: EmulationSideArtwork) = viewModelScope.launch {
        if (artwork != EmulationSideArtwork.CUSTOM || emulationSideArtworkRepository.existingCustomFile() != null) {
            preferences.setEmulationSideArtwork(artwork)
        }
    }

    fun installEmulationSideArtwork(uri: Uri) = viewModelScope.launch {
        _uiState.value = _uiState.value.copy(
            isSideArtworkImporting = true,
            customizationMessageResId = null
        )
        val result = emulationSideArtworkRepository.install(uri)
        if (result.isSuccess) {
            preferences.setEmulationSideArtwork(EmulationSideArtwork.CUSTOM)
        }
        _uiState.value = _uiState.value.copy(
            isSideArtworkImporting = false,
            customizationMessageResId = if (result.isSuccess) {
                com.sbro.emucorer.R.string.settings_customization_side_artwork_applied
            } else {
                com.sbro.emucorer.R.string.settings_customization_side_artwork_failed
            }
        )
    }

    fun clearCustomEmulationSideArtwork() = viewModelScope.launch(Dispatchers.IO) {
        emulationSideArtworkRepository.clear()
        preferences.setEmulationSideArtwork(EmulationSideArtwork.NONE)
        _uiState.value = _uiState.value.copy(
            customizationMessageResId = com.sbro.emucorer.R.string.settings_customization_side_artwork_removed
        )
    }

    fun refreshShaderPresets() = viewModelScope.launch(Dispatchers.IO) {
        val presets = retroArchShaderRepository.listPresets()
        _uiState.value = _uiState.value.copy(
            shaderPresets = presets,
            isShaderPackInstalled = retroArchShaderRepository.hasInstalledPack()
        )
    }

    fun setShaderChainEnabled(enabled: Boolean) = viewModelScope.launch {
        val preset = _uiState.value.shaderChainPreset
        preferences.setShaderChain(enabled, preset)
        EmulatorBridge.setSetting("EmuCore/GS", "ShaderChainEnabled", "bool", enabled.toString())
        EmulatorBridge.setSetting("EmuCore/GS", "ShaderChainPreset", "string", preset)
    }

    fun setShaderChainPreset(path: String) = viewModelScope.launch {
        val enabled = path.isNotBlank() && _uiState.value.shaderChainEnabled
        preferences.setShaderChain(enabled, path)
        EmulatorBridge.setSetting("EmuCore/GS", "ShaderChainEnabled", "bool", enabled.toString())
        EmulatorBridge.setSetting("EmuCore/GS", "ShaderChainPreset", "string", path)
    }

    fun downloadOfficialShaderPack() {
        if (_uiState.value.isShaderPackInstalled) return
        installShaderPack(
            initialProgress = ShaderPackInstallProgress(ShaderPackInstallStage.DOWNLOADING)
        ) { onProgress ->
            retroArchShaderRepository.downloadOfficialPack(onProgress)
        }
    }

    fun importShaderPack(uri: Uri) = installShaderPack(
        initialProgress = ShaderPackInstallProgress(ShaderPackInstallStage.INSTALLING)
    ) {
        retroArchShaderRepository.importArchive(uri)
    }

    private fun installShaderPack(
        initialProgress: ShaderPackInstallProgress,
        block: ((ShaderPackInstallProgress) -> Unit) -> Result<Int>
    ) = viewModelScope.launch(Dispatchers.IO) {
        if (_uiState.value.isShaderPackBusy) return@launch
        _uiState.value = _uiState.value.copy(
            isShaderPackBusy = true,
            shaderPackProgress = initialProgress,
            shaderPackMessageResId = null
        )
        val result = block { progress ->
            _uiState.value = _uiState.value.copy(shaderPackProgress = progress)
        }
        val presets = retroArchShaderRepository.listPresets()
        _uiState.value = _uiState.value.copy(
            isShaderPackBusy = false,
            shaderPackProgress = null,
            shaderPresets = presets,
            isShaderPackInstalled = retroArchShaderRepository.hasInstalledPack(),
            shaderPackMessageResId = if (result.isSuccess) {
                com.sbro.emucorer.R.string.settings_shader_pack_installed
            } else {
                com.sbro.emucorer.R.string.settings_shader_pack_failed
            }
        )
    }

    fun clearShaderPackMessage() {
        _uiState.value = _uiState.value.copy(shaderPackMessageResId = null)
    }

    fun refreshPatchDatabaseState() = viewModelScope.launch(Dispatchers.IO) {
        _uiState.value = _uiState.value.copy(
            installedPatchCount = patchDatabaseDownloader.installedPatchCount()
        )
    }

    fun downloadPatchDatabase() = viewModelScope.launch(Dispatchers.IO) {
        if (_uiState.value.isPatchDatabaseBusy) return@launch
        if (!patchDatabaseDownloader.hasConfiguredSource()) {
            _uiState.value = _uiState.value.copy(
                patchDatabaseMessageResId = com.sbro.emucorer.R.string.settings_patches_source_missing,
                patchDatabaseMessageCount = null
            )
            return@launch
        }
        _uiState.value = _uiState.value.copy(
            isPatchDatabaseBusy = true,
            patchDatabaseProgress = PatchDatabaseInstallProgress(PatchDatabaseInstallStage.DOWNLOADING),
            patchDatabaseMessageResId = null,
            patchDatabaseMessageCount = null
        )
        val result = patchDatabaseDownloader.download { progress ->
            _uiState.value = _uiState.value.copy(patchDatabaseProgress = progress)
        }
        val installedCount = patchDatabaseDownloader.installedPatchCount()
        _uiState.value = _uiState.value.copy(
            isPatchDatabaseBusy = false,
            patchDatabaseProgress = null,
            installedPatchCount = installedCount,
            patchDatabaseMessageResId = if (result.isSuccess) {
                com.sbro.emucorer.R.string.settings_patches_download_success
            } else {
                com.sbro.emucorer.R.string.settings_patches_download_failed
            },
            patchDatabaseMessageCount = installedCount.takeIf { result.isSuccess }
        )
        if (result.isSuccess) {
            preferences.bumpPatchDatabaseRevision()
            EmulatorBridge.reloadPatches()
        }
    }

    fun clearPatchDatabaseMessage() {
        _uiState.value = _uiState.value.copy(
            patchDatabaseMessageResId = null,
            patchDatabaseMessageCount = null
        )
    }

    fun setPatchDatabaseUseOfficial(enabled: Boolean) = viewModelScope.launch {
        preferences.setPatchDatabaseUseOfficial(enabled)
    }

    fun setPatchDatabaseCustomUrl(url: String?) = viewModelScope.launch {
        preferences.setPatchDatabaseCustomUrl(url)
    }

    fun resetCustomization() = viewModelScope.launch(Dispatchers.IO) {
        homeBackgroundRepository.clear()
        emulationSideArtworkRepository.clear()
        customFontRepository.clear()
        preferences.setHomeBackgroundType(HomeBackgroundType.NONE)
        preferences.setHomeBackgroundDim(AppPreferences.DEFAULT_HOME_BACKGROUND_DIM)
        preferences.setEmulationSideArtwork(EmulationSideArtwork.NONE)
        preferences.setEmulationSideArtworkDim(AppPreferences.DEFAULT_EMULATION_SIDE_ARTWORK_DIM)
        preferences.setHomeGridScale(AppPreferences.DEFAULT_HOME_GRID_SCALE)
        preferences.setAppFontChoice(AppFontChoice.SYSTEM)
        preferences.clearCustomFont()
        preferences.setAppFontScale(AppPreferences.DEFAULT_APP_FONT_SCALE)
        preferences.setTouchControlVisualStyle(TouchControlVisualStyle.CLASSIC)
        preferences.setTouchControlPressEffect(TouchControlPressEffect.GROW)
        preferences.setDrawerVisualStyle(DrawerVisualStyle.CLASSIC)
        preferences.setHiddenDrawerItems(emptySet())
        preferences.setCustomTheme(CustomThemeConfig.Default)
        _uiState.value = _uiState.value.copy(
            customizationMessageResId = com.sbro.emucorer.R.string.settings_customization_reset_done
        )
    }

    fun clearCustomizationMessage() {
        _uiState.value = _uiState.value.copy(customizationMessageResId = null)
    }
    fun setLanguage(tag: String?) { viewModelScope.launch { preferences.setLanguageTag(tag) } }

    fun setRenderer(value: Int) {
        viewModelScope.launch {
            if (!EmulatorBridge.setRenderer(value)) return@launch
            preferences.setRenderer(value)
        }
    }

    fun loadAppReleaseHistory(showErrors: Boolean = true, force: Boolean = false) {
        if (_uiState.value.appUpdate.historyLoading) return
        viewModelScope.launch(Dispatchers.IO) {
            _uiState.value = _uiState.value.copy(
                appUpdate = _uiState.value.appUpdate.copy(
                    historyLoading = true,
                    historyErrorMessage = null
                )
            )
            runCatching {
                appUpdateRepository.loadReleaseHistory(force)
            }.onSuccess { releases ->
                _uiState.value = _uiState.value.copy(
                    appUpdate = _uiState.value.appUpdate.copy(
                        releaseHistory = releases,
                        historyLoading = false,
                        historyErrorMessage = null
                    )
                )
            }.onFailure { error ->
                val errorMsg = if (showErrors) {
                    if (error is com.sbro.emucorer.core.RateLimitException) {
                        val minutes = ((error.resetTimestampMs - System.currentTimeMillis()) / 60000).coerceAtLeast(1)
                        getApplication<Application>().getString(com.sbro.emucorer.R.string.settings_updates_rate_limit_error, minutes)
                    } else {
                        error.message ?: "Could not load release history"
                    }
                } else null

                _uiState.value = _uiState.value.copy(
                    appUpdate = _uiState.value.appUpdate.copy(
                        historyLoading = false,
                        historyErrorMessage = errorMsg
                    )
                )
            }
        }
    }
    fun setUpscaleMultiplier(value: Float) {
        viewModelScope.launch {
            val normalizedValue = normalizeUpscale(value)
            preferences.setUpscaleMultiplier(normalizedValue)
            EmulatorBridge.setUpscaleMultiplier(normalizedValue)
        }
    }

    fun setAspectRatio(value: Int) {
        viewModelScope.launch {
            preferences.setAspectRatio(value)
            EmulatorBridge.setAspectRatio(value)
        }
    }

    fun setDisplayCrop(value: DisplayCrop) {
        viewModelScope.launch {
            val crop = value.sanitized()
            preferences.setDisplayCrop(crop)
            EmulatorBridge.setDisplayCrop(crop)
        }
    }

    fun setPadVibration(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setPadVibration(enabled)
            EmulatorBridge.setPadVibration(enabled)
        }
    }

    fun setPadVibrationStrength(value: Int) {
        viewModelScope.launch {
            preferences.setPadVibrationStrength(value)
        }
    }

    fun setPadVibrationFallback(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setPadVibrationFallback(enabled)
        }
    }

    fun testPadVibration(
        strengthPercent: Int = _uiState.value.padVibrationStrength,
        durationMs: Long = 260L
    ) {
        GamepadManager.ensureInitialized(getApplication())
        GamepadManager.testPadVibration(
            padIndex = 0,
            strengthPercent = strengthPercent,
            durationMs = durationMs
        )
    }

    fun setGamepadBinding(padIndex: Int, actionId: String, keyCode: Int) {
        viewModelScope.launch {
            preferences.setGamepadBinding(padIndex, actionId, keyCode)
        }
    }

    fun clearGamepadBinding(padIndex: Int, actionId: String) {
        viewModelScope.launch {
            preferences.clearGamepadBinding(padIndex, actionId)
        }
    }

    fun resetGamepadBindingsForPad(padIndex: Int) {
        viewModelScope.launch {
            preferences.resetGamepadBindingsForPad(padIndex)
        }
    }

    fun setGamepadDeviceAssignment(padIndex: Int, deviceKey: String?) {
        viewModelScope.launch { preferences.setGamepadDeviceAssignment(padIndex, deviceKey) }
    }

    fun setGamepadDeviceIgnored(deviceKey: String, ignored: Boolean) {
        viewModelScope.launch { preferences.setGamepadDeviceIgnored(deviceKey, ignored) }
    }

    fun resetGamepadDeviceAssignments() {
        viewModelScope.launch { preferences.resetGamepadDeviceAssignments() }
    }

    fun resetAllSettings() {
        viewModelScope.launch {
            preferences.resetAllSettings()
        }
    }

    fun setEnableCheats(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setEnableCheats(enabled)
            EmulatorBridge.setSetting("EmuCore", "EnableCheats", "bool", enabled.toString())
            if (enabled) {
                EmulatorBridge.reloadPatches()
            }
        }
    }

    fun setShowFps(enabled: Boolean) { viewModelScope.launch { preferences.setShowFps(enabled) } }
    fun setAudioVolume(value: Int) {
        viewModelScope.launch {
            val normalized = AudioDefaults.coerceVolume(value)
            preferences.setAudioVolume(normalized)
            EmulatorBridge.setSetting("Audio/Output", "Volume", "int", normalized.toString())
        }
    }

    fun setAudioMuted(muted: Boolean) {
        viewModelScope.launch {
            preferences.setAudioMuted(muted)
            EmulatorBridge.setSetting("Audio/Output", "Mute", "bool", muted.toString())
        }
    }

    fun setAudioOutputLatencyMs(value: Int) {
        viewModelScope.launch {
            val normalized = AudioDefaults.coerceOutputLatencyMs(value)
            preferences.setAudioOutputLatencyMs(normalized)
            EmulatorBridge.setSetting("Audio/Output", "AudioOutputLatencyMs", "int", normalized.toString())
        }
    }

    fun setAudioMinimalOutputLatency(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setAudioMinimalOutputLatency(enabled)
            EmulatorBridge.setSetting("Audio/Output", "MinimalOutputLatency", "bool", enabled.toString())
        }
    }

    fun setFpsOverlayMode(mode: Int) { viewModelScope.launch { preferences.setFpsOverlayMode(mode) } }
    fun setFpsOverlayCorner(corner: Int) { viewModelScope.launch { preferences.setFpsOverlayCorner(corner) } }
    fun setFpsOverlayScale(scale: Int) { viewModelScope.launch { preferences.setFpsOverlayScale(scale) } }
    fun setFpsOverlayMetrics(metrics: Int) { viewModelScope.launch { preferences.setFpsOverlayMetrics(metrics) } }
    fun setConfirmSaveLoadActions(enabled: Boolean) { viewModelScope.launch { preferences.setConfirmSaveLoadActions(enabled) } }
    fun setFloatingQuickActionsEnabled(enabled: Boolean) {
        viewModelScope.launch { preferences.setFloatingQuickActionsEnabled(enabled) }
    }

    fun setOrientationLock(value: Int) {
        viewModelScope.launch { preferences.setOrientationLock(value) }
    }

    fun setEmulationAllowsBothOrientations(enabled: Boolean) {
        viewModelScope.launch { preferences.setEmulationAllowsBothOrientations(enabled) }
    }
    fun setBackButtonExitsGame(enabled: Boolean) { viewModelScope.launch { preferences.setBackButtonExitsGame(enabled) } }
    fun setKeepScreenOn(enabled: Boolean) { viewModelScope.launch { preferences.setKeepScreenOn(enabled) } }

    fun setRespectDisplayCutout(enabled: Boolean) {
        viewModelScope.launch { preferences.setRespectDisplayCutout(enabled) }
    }
    fun setTvInterfaceMode(mode: TvInterfaceMode) {
        viewModelScope.launch { preferences.setTvInterfaceMode(mode) }
    }
    fun setRacingMode(enabled: Boolean) { viewModelScope.launch { preferences.setRacingMode(enabled) } }
    fun setStickyButtons(buttons: Set<String>) {
        viewModelScope.launch { preferences.setStickyButtons(buttons) }
    }
    fun setTouchHaptics(enabled: Boolean) { viewModelScope.launch { preferences.setTouchHaptics(enabled) } }
    fun setTouchscreenRightStick(enabled: Boolean) { viewModelScope.launch { preferences.setTouchscreenRightStick(enabled) } }
    fun setTouchscreenRightStickSensitivity(value: Int) {
        viewModelScope.launch { preferences.setTouchscreenRightStickSensitivity(value) }
    }
    fun setTouchHapticsPreset(value: Int) { viewModelScope.launch { preferences.setTouchHapticsPreset(value) } }
    fun setTouchHapticsStrength(value: Int) { viewModelScope.launch { preferences.setTouchHapticsStrength(value) } }
    fun setGyroMode(value: Int) { viewModelScope.launch { preferences.setGyroMode(value) } }
    fun setGyroSensitivity(value: Int) { viewModelScope.launch { preferences.setGyroSensitivity(value) } }
    fun setGyroSmoothing(value: Int) { viewModelScope.launch { preferences.setGyroSmoothing(value) } }
    fun setGyroInvertX(value: Boolean) { viewModelScope.launch { preferences.setGyroInvertX(value) } }
    fun setGyroInvertY(value: Boolean) { viewModelScope.launch { preferences.setGyroInvertY(value) } }
    fun testTouchHaptics(
        strengthPercent: Int = _uiState.value.touchHapticsStrength,
        preset: Int = _uiState.value.touchHapticsPreset
    ) {
        viewModelScope.launch {
            AndroidTouchHaptics.playButton(
                context = getApplication(),
                strengthPercent = strengthPercent,
                preset = preset,
                phase = AndroidTouchHaptics.ButtonPhase.PRESS
            )
            delay(85.milliseconds)
            AndroidTouchHaptics.playButton(
                context = getApplication(),
                strengthPercent = strengthPercent,
                preset = preset,
                phase = AndroidTouchHaptics.ButtonPhase.RELEASE
            )
        }
    }
    fun setShowRecentGames(enabled: Boolean) { viewModelScope.launch { preferences.setShowRecentGames(enabled) } }
    fun setShowHomeSearch(enabled: Boolean) { viewModelScope.launch { preferences.setShowHomeSearch(enabled) } }
    fun setShowDebugOptions(enabled: Boolean) { viewModelScope.launch { preferences.setShowDebugOptions(enabled) } }
    fun setPreferEnglishGameTitles(enabled: Boolean) {
        viewModelScope.launch {
            EmulatorBridge.setSetting("UI", "PreferEnglishGameTitles", "bool", enabled.toString())
            preferences.setPreferEnglishGameTitles(enabled)
        }
    }

    fun setEnableFastBoot(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setEnableFastBoot(enabled)
            EmulatorBridge.setSetting("EmuCore", "EnableFastBoot", "bool", enabled.toString())
        }
    }

    fun setFrameSkip(value: Int) {
        viewModelScope.launch {
            preferences.setFrameSkip(value)
            EmulatorBridge.setFrameSkip(value)
        }
    }

    fun setFrameLimitEnabled(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setFrameLimitEnabled(enabled)
            EmulatorBridge.setFrameLimitEnabled(enabled)
        }
    }

    fun setTargetFps(value: Int) {
        viewModelScope.launch {
            preferences.setTargetFps(if (value <= 0) 0 else value)
            EmulatorBridge.setTargetFps(value)
        }
    }

    fun setTextureFiltering(value: Int) {
        viewModelScope.launch {
            preferences.setTextureFiltering(value)
            EmulatorBridge.setSetting("EmuCore/GS", "filter", "int", value.toString())
        }
    }

    fun setEnableWidescreenPatches(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setEnableWidescreenPatches(enabled)
            EmulatorBridge.setSetting("EmuCore", "EnableWideScreenPatches", "bool", enabled.toString())
            NativeApp.reloadPatches()
        }
    }

    fun setEnableIcacheEmulation(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setEnableIcacheEmulation(enabled)
        }
    }

    fun setEnableCddaAudio(enabled: Boolean) {
        viewModelScope.launch {
            preferences.setEnableCddaAudio(enabled)
        }
    }

    fun setMultitapMode(value: Int) {
        viewModelScope.launch {
            preferences.setMultitapMode(value)
        }
    }

    fun setCdReadAhead(value: Int) {
        viewModelScope.launch {
            preferences.setCdReadAhead(value)
        }
    }

    // Overlay
    fun setOverlayScale(value: Int) { viewModelScope.launch { preferences.setOverlayScale(value) } }
    fun setOverlayOpacity(value: Int) { viewModelScope.launch { preferences.setOverlayOpacity(value) } }
    fun setLeftStickSensitivity(value: Int) { viewModelScope.launch { preferences.setLeftStickSensitivity(value) } }
    fun setRightStickSensitivity(value: Int) { viewModelScope.launch { preferences.setRightStickSensitivity(value) } }
    fun setInvertLeftStick(enabled: Boolean) { viewModelScope.launch { preferences.setInvertLeftStick(enabled) } }
    fun setInvertRightStick(enabled: Boolean) { viewModelScope.launch { preferences.setInvertRightStick(enabled) } }
    fun setInvertLeftStickHorizontal(enabled: Boolean) { viewModelScope.launch { preferences.setInvertLeftStickHorizontal(enabled) } }
    fun setInvertRightStickHorizontal(enabled: Boolean) { viewModelScope.launch { preferences.setInvertRightStickHorizontal(enabled) } }

    // Gamepad
    fun setEnableAutoGamepad(enabled: Boolean) { viewModelScope.launch { preferences.setEnableAutoGamepad(enabled) } }
    fun setHideOverlayOnGamepad(enabled: Boolean) { viewModelScope.launch { preferences.setHideOverlayOnGamepad(enabled) } }
    fun setGamepadStickDeadzone(value: Int) { viewModelScope.launch { preferences.setGamepadStickDeadzone(value) } }
    fun setGamepadLeftStickSensitivity(value: Int) { viewModelScope.launch { preferences.setGamepadLeftStickSensitivity(value) } }
    fun setGamepadRightStickSensitivity(value: Int) { viewModelScope.launch { preferences.setGamepadRightStickSensitivity(value) } }
    fun setGamepadRightStickUpToR2(enabled: Boolean) { viewModelScope.launch { preferences.setGamepadRightStickUpToR2(enabled) } }
    fun setGamepadRightStickDownToL2(enabled: Boolean) { viewModelScope.launch { preferences.setGamepadRightStickDownToL2(enabled) } }
    fun setGamepadButtonHaptics(enabled: Boolean) { viewModelScope.launch { preferences.setGamepadButtonHaptics(enabled) } }
    fun setBiosPath(uri: Uri) {
        val application = getApplication<Application>()
        viewModelScope.launch(Dispatchers.IO) {
            val previousPath = preferences.biosPath.first()
            StorageAccess.takePersistableReadPermission(application, uri)
            preferences.setBiosPath(uri.toString())
            if (previousPath != uri.toString()) {
                StorageAccess.releasePersistedPermission(application, previousPath)
            }
            EmulatorBridge.applyRuntimeConfig(
                biosPath = uri.toString(),
                emulatorDataPath = _uiState.value.emulatorDataPath,
                memoryCardSlot1 = preferences.memoryCardSlot1.first(),
                memoryCardSlot2 = preferences.memoryCardSlot2.first(),
                renderer = _uiState.value.renderer,
                upscaleMultiplier = _uiState.value.upscaleMultiplier,
                aspectRatio = _uiState.value.aspectRatio,
                displayCrop = _uiState.value.displayCrop,
                audioVolume = _uiState.value.audioVolume,
                audioMuted = _uiState.value.audioMuted,
                audioOutputLatencyMs = _uiState.value.audioOutputLatencyMs,
                audioMinimalOutputLatency = _uiState.value.audioMinimalOutputLatency,
                enableFastBoot = _uiState.value.enableFastBoot,
                frameLimitEnabled = _uiState.value.frameLimitEnabled,
                targetFps = _uiState.value.targetFps,
                textureFiltering = _uiState.value.textureFiltering,
                shaderChainEnabled = _uiState.value.shaderChainEnabled,
                shaderChainPreset = _uiState.value.shaderChainPreset,
                widescreenPatches = _uiState.value.enableWidescreenPatches,
                enableIcacheEmulation = _uiState.value.enableIcacheEmulation,
                cdReadAhead = _uiState.value.cdReadAhead,
                enableCddaAudio = _uiState.value.enableCddaAudio,
                multitapMode = _uiState.value.multitapMode
            )
        }
    }

    fun setGamePath(uri: Uri) {
        val application = getApplication<Application>()
        viewModelScope.launch(Dispatchers.IO) {
            StorageAccess.takePersistableReadPermission(application, uri)
            val rawPath = uri.toString()
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
                _uiState.value = _uiState.value.copy(sdCardDataPath = sdCardDataPath)
            }
        }
    }

    fun setCoverDownloadBaseUrl(url: String?) {
        viewModelScope.launch {
            preferences.setCoverDownloadBaseUrl(url)
            CoverArtRepository(getApplication()).clearCache()
            clearCoverImageMemoryCache()
        }
    }

    fun setCoverArtStyle(style: Int) {
        viewModelScope.launch {
            preferences.setCoverArtStyle(style)
            CoverArtRepository(getApplication()).clearCache()
            clearCoverImageMemoryCache()
        }
    }

    fun clearCoverCache(onComplete: (CoverCacheClearResult) -> Unit) {
        viewModelScope.launch(Dispatchers.IO) {
            val result = CoverArtRepository(getApplication()).clearAllTemporaryImageCaches()
            clearCoverImageMemoryCache()
            preferences.notifyCoverCacheCleared()
            withContext(Dispatchers.Main) {
                onComplete(result)
            }
        }
    }

    private companion object {
        const val CORE_NAME = "SwanStation"
        const val CORE_VERSION = "1.0.0"
    }
}
