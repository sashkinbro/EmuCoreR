package com.sbro.emucorer.ui.ps1shell

import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.opengl.GLSurfaceView
import android.os.BatteryManager
import android.os.Build
import android.os.Environment
import android.os.StatFs
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.sbro.emucorer.BuildConfig
import com.sbro.emucorer.MainActivity
import com.sbro.emucorer.R
import com.sbro.emucorer.core.BiosValidator
import com.sbro.emucorer.core.UiSoundManager
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.data.CoverArtRepository
import com.sbro.emucorer.data.CustomGameCoverRepository
import com.sbro.emucorer.data.GameItem
import com.sbro.emucorer.data.MemoryCardInfo
import com.sbro.emucorer.data.MemoryCardRepository
import com.sbro.emucorer.data.SaveStateEntryInfo
import com.sbro.emucorer.data.SaveStateRepository
import com.sbro.emucorer.ps1ui.Ps1ShellAudio
import com.sbro.emucorer.ps1ui.Ps1ShellHost
import com.sbro.emucorer.ps1ui.Ps1ShellNative
import com.sbro.emucorer.ps1ui.Ps1ShellSurfaceRenderer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs

private data class Ps1ResolvedGame(
    val title: String,
    val path: String,
    val serial: String,
    val coverPath: String,
    val sizeStr: String,
    val dateStr: String,
    val saveCount: Int,
    val latestSaveSlot: Int,
    val discCount: Int,
    val discLabelsPacked: String,
    val discPathsPacked: String,
)

private data class Ps1MemoryCardBundle(
    val slot1Name: String = "",
    val slot1Size: String = "",
    val slot1Status: String = "",
    val slot2Name: String = "",
    val slot2Size: String = "",
    val slot2Status: String = "",
    val cards: List<MemoryCardInfo> = emptyList(),
)

private data class Ps1SystemSnapshot(
    val batteryLevel: Int = 100,
    val batteryCharging: Boolean = false,
    val wifiConnected: Boolean = false,
    val bluetoothConnected: Boolean = false,
    val audioMuted: Boolean = false,
    val freeStorage: String = "",
    val deviceName: String = "",
    val appVersion: String = BuildConfig.VERSION_NAME,
    val biosStatus: String = "",
    val biosValid: Boolean = false,
)

private object Ps1ShellSessionCache {
    @Volatile
    var lastSaves: List<SaveStateEntryInfo> = emptyList()

    @Volatile
    var hasLoadedSaves: Boolean = false

    @Volatile
    var returnedFromGame: Boolean = false

    @Volatile
    var lastBiosValid: Boolean = false

    @Volatile
    var lastSystemSnapshot: Ps1SystemSnapshot = Ps1SystemSnapshot()
}

private val PS1_STRING_BINDINGS: List<Pair<String, Int>> = listOf(
    "ps1ui_main_menu" to R.string.ps1ui_main_menu,
    "ps1ui_menu_games" to R.string.ps1ui_menu_games,
    "ps1ui_menu_memory_card" to R.string.ps1ui_menu_memory_card,
    "ps1ui_menu_settings" to R.string.ps1ui_menu_settings,
    "ps1ui_boot_subtitle" to R.string.ps1ui_boot_subtitle,
    "ps1ui_hint_select" to R.string.ps1ui_hint_select,
    "ps1ui_hint_exit" to R.string.ps1ui_hint_exit,
    "ps1ui_hint_back" to R.string.ps1ui_hint_back,
    "ps1ui_hint_tab" to R.string.ps1ui_hint_tab,
    "ps1ui_hint_change" to R.string.ps1ui_hint_change,
    "ps1ui_btn_start" to R.string.ps1ui_btn_start,
    "ps1ui_btn_continue" to R.string.ps1ui_btn_continue,
    "ps1ui_btn_options" to R.string.ps1ui_btn_options,
    "ps1ui_loading_games" to R.string.ps1ui_loading_games,
    "ps1ui_no_games" to R.string.ps1ui_no_games,
    "ps1ui_disc" to R.string.ps1ui_disc,
    "ps1ui_discs" to R.string.ps1ui_discs,
    "ps1ui_quick_saves_lbl" to R.string.ps1ui_quick_saves_lbl,
    "ps1ui_mc_tab_all" to R.string.ps1ui_mc_tab_all,
    "ps1ui_mc_tab_game" to R.string.ps1ui_mc_tab_game,
    "ps1ui_mc_tab_cards" to R.string.ps1ui_mc_tab_cards,
    "ps1ui_mc_slot1" to R.string.ps1ui_mc_slot1,
    "ps1ui_mc_slot2" to R.string.ps1ui_mc_slot2,
    "ps1ui_mc_empty_slot" to R.string.ps1ui_mc_empty_slot,
    "ps1ui_mc_available_cards" to R.string.ps1ui_mc_available_cards,
    "ps1ui_mc_blocks_title" to R.string.ps1ui_mc_blocks_title,
    "ps1ui_saves_word" to R.string.ps1ui_saves_word,
    "ps1ui_mc_free_block" to R.string.ps1ui_mc_free_block,
    "ps1ui_loading_saves" to R.string.ps1ui_loading_saves,
    "ps1ui_mc_no_saves" to R.string.ps1ui_mc_no_saves,
    "ps1ui_mc_empty_block" to R.string.ps1ui_mc_empty_block,
    "ps1ui_mc_load" to R.string.ps1ui_mc_load,
    "ps1ui_mc_delete" to R.string.ps1ui_mc_delete,
    "ps1ui_mc_exit" to R.string.ps1ui_mc_exit,
    "ps1ui_theme_classic" to R.string.ps1ui_theme_classic,
    "ps1ui_theme_psone" to R.string.ps1ui_theme_psone,
    "ps1ui_theme_slate" to R.string.ps1ui_theme_slate,
    "ps1ui_theme_yaroze" to R.string.ps1ui_theme_yaroze,
    "ps1ui_theme_crimson" to R.string.ps1ui_theme_crimson,
    "ps1ui_aspect_auto" to R.string.ps1ui_aspect_auto,
    "ps1ui_aspect_16_9" to R.string.ps1ui_aspect_16_9,
    "ps1ui_aspect_4_3" to R.string.ps1ui_aspect_4_3,
    "ps1ui_orbs_normal" to R.string.ps1ui_orbs_normal,
    "ps1ui_orbs_calm" to R.string.ps1ui_orbs_calm,
    "ps1ui_orbs_static" to R.string.ps1ui_orbs_static,
    "ps1ui_scan_subtle" to R.string.ps1ui_scan_subtle,
    "ps1ui_scan_classic" to R.string.ps1ui_scan_classic,
    "ps1ui_boot_full" to R.string.ps1ui_boot_full,
    "ps1ui_boot_fast" to R.string.ps1ui_boot_fast,
    "ps1ui_on" to R.string.ps1ui_on,
    "ps1ui_off" to R.string.ps1ui_off,
    "ps1ui_btn_cross" to R.string.ps1ui_btn_cross,
    "ps1ui_btn_circle" to R.string.ps1ui_btn_circle,
    "ps1ui_time_24h" to R.string.ps1ui_time_24h,
    "ps1ui_time_12h" to R.string.ps1ui_time_12h,
    "ps1ui_open" to R.string.ps1ui_open,
    "ps1ui_view" to R.string.ps1ui_view,
    "ps1ui_reset" to R.string.ps1ui_reset,
    "ps1ui_set_aspect" to R.string.ps1ui_set_aspect,
    "ps1ui_set_theme" to R.string.ps1ui_set_theme,
    "ps1ui_set_orbs" to R.string.ps1ui_set_orbs,
    "ps1ui_set_scanlines" to R.string.ps1ui_set_scanlines,
    "ps1ui_set_boot_anim" to R.string.ps1ui_set_boot_anim,
    "ps1ui_set_key_tone" to R.string.ps1ui_set_key_tone,
    "ps1ui_set_ambient" to R.string.ps1ui_set_ambient,
    "ps1ui_set_confirm_btn" to R.string.ps1ui_set_confirm_btn,
    "ps1ui_set_touch" to R.string.ps1ui_set_touch,
    "ps1ui_set_touch_guide" to R.string.ps1ui_set_touch_guide,
    "ps1ui_set_time_fmt" to R.string.ps1ui_set_time_fmt,
    "ps1ui_set_date_fmt" to R.string.ps1ui_set_date_fmt,
    "ps1ui_set_keep_screen" to R.string.ps1ui_set_keep_screen,
    "ps1ui_set_sysinfo" to R.string.ps1ui_set_sysinfo,
    "ps1ui_set_restore" to R.string.ps1ui_set_restore,
    "ps1ui_launching_save" to R.string.ps1ui_launching_save,
    "ps1ui_launching_disc" to R.string.ps1ui_launching_disc,
    "ps1ui_exit_title" to R.string.ps1ui_exit_title,
    "ps1ui_exit_msg" to R.string.ps1ui_exit_msg,
    "ps1ui_del_title" to R.string.ps1ui_del_title,
    "ps1ui_del_msg" to R.string.ps1ui_del_msg,
    "ps1ui_restore_title" to R.string.ps1ui_restore_title,
    "ps1ui_restore_msg" to R.string.ps1ui_restore_msg,
    "ps1ui_yes" to R.string.ps1ui_yes,
    "ps1ui_no" to R.string.ps1ui_no,
    "ps1ui_opt_start" to R.string.ps1ui_opt_start,
    "ps1ui_opt_continue" to R.string.ps1ui_opt_continue,
    "ps1ui_opt_saves" to R.string.ps1ui_opt_saves,
    "ps1ui_opt_info" to R.string.ps1ui_opt_info,
    "ps1ui_opt_settings" to R.string.ps1ui_opt_settings,
    "ps1ui_select_disc" to R.string.ps1ui_select_disc,
    "ps1ui_info_title" to R.string.ps1ui_info_title,
    "ps1ui_info_serial" to R.string.ps1ui_info_serial,
    "ps1ui_info_size" to R.string.ps1ui_info_size,
    "ps1ui_info_discs" to R.string.ps1ui_info_discs,
    "ps1ui_close" to R.string.ps1ui_close,
    "ps1ui_sys_app" to R.string.ps1ui_sys_app,
    "ps1ui_sys_device" to R.string.ps1ui_sys_device,
    "ps1ui_sys_bios" to R.string.ps1ui_sys_bios,
    "ps1ui_sys_storage" to R.string.ps1ui_sys_storage,
    "ps1ui_touch_guide_title" to R.string.ps1ui_touch_guide_title,
    "ps1ui_touch_tip_1" to R.string.ps1ui_touch_tip_1,
    "ps1ui_touch_tip_2" to R.string.ps1ui_touch_tip_2,
    "ps1ui_touch_tip_3" to R.string.ps1ui_touch_tip_3,
    "ps1ui_touch_tip_4" to R.string.ps1ui_touch_tip_4,
    "ps1ui_got_it" to R.string.ps1ui_got_it,
)

private fun normalizeSerialKey(raw: String?): String? {
    if (raw.isNullOrBlank()) return null
    val upper = raw.trim().uppercase(Locale.ROOT)
    val dashPat = Regex("([A-Z]{4})[^A-Z0-9]*(\\d{3})[^A-Z0-9]*(\\d{2})")
    dashPat.find(upper)?.let { return "${it.groupValues[1]}-${it.groupValues[2]}${it.groupValues[3]}" }
    val compactPat = Regex("([A-Z]{4})[^A-Z0-9]*(\\d{5})")
    compactPat.find(upper)?.let { return "${it.groupValues[1]}-${it.groupValues[2]}" }
    return upper.replace(Regex("[^A-Z0-9]"), "").takeIf { it.isNotBlank() }
}

private fun formatFileSize(sizeBytes: Long): String = when {
    sizeBytes <= 0L -> ""
    sizeBytes < 1024L * 1024L -> "${((sizeBytes + 1023L) / 1024L).coerceAtLeast(1L)} KB"
    else -> String.format(Locale.US, "%.1f MB", sizeBytes / (1024.0 * 1024.0))
}

private fun formatTimestamp(ms: Long): String = if (ms > 0L) {
    SimpleDateFormat("dd/MM/yyyy HH:mm", Locale.getDefault()).format(Date(ms))
} else {
    ""
}

private fun hasPhysicalGamepadConnected(): Boolean = runCatching {
    InputDevice.getDeviceIds().any { id ->
        val dev = InputDevice.getDevice(id) ?: return@any false
        if (dev.isVirtual) return@any false
        val sources = dev.sources
        val isGamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        isGamepad && dev.hasKeys(
            KeyEvent.KEYCODE_BUTTON_A,
            KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_DPAD_UP,
        ).any { it }
    }
}.getOrDefault(false)

private fun readSystemSnapshot(context: Context, biosValid: Boolean): Ps1SystemSnapshot {
    val batteryIntent = runCatching {
        context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    }.getOrNull()
    val lvl = batteryIntent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
    val scale = batteryIntent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
    val status = batteryIntent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
    val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
        status == BatteryManager.BATTERY_STATUS_FULL
    val pct = if (scale > 0 && lvl >= 0) (lvl * 100) / scale else 100

    val wifiConnected = runCatching {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
        val net = cm?.activeNetwork
        val caps = if (net != null) cm.getNetworkCapabilities(net) else null
        caps?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true
    }.getOrDefault(false)

    val btEnabled = runCatching {
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter?.isEnabled == true
    }.getOrDefault(false)
    val btAudio = runCatching {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
        am?.getDevices(AudioManager.GET_DEVICES_OUTPUTS)?.any { dev ->
            dev.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                dev.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
                dev.type == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                dev.type == AudioDeviceInfo.TYPE_BLE_SPEAKER
        } == true
    }.getOrDefault(false)
    val muted = runCatching {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
        (am?.getStreamVolume(AudioManager.STREAM_MUSIC) ?: 1) <= 0
    }.getOrDefault(false)

    val freeText = runCatching {
        val stat = StatFs(Environment.getDataDirectory().path)
        val avail = stat.availableBytes
        if (avail >= 1024L * 1024L * 1024L) {
            String.format(Locale.US, "%.1f GB", avail / (1024.0 * 1024.0 * 1024.0))
        } else {
            "${(avail / (1024L * 1024L)).coerceAtLeast(0L)} MB"
        }
    }.getOrDefault("")

    return Ps1SystemSnapshot(
        batteryLevel = pct,
        batteryCharging = charging,
        wifiConnected = wifiConnected,
        bluetoothConnected = btEnabled && btAudio,
        audioMuted = muted,
        freeStorage = freeText,
        deviceName = Build.MODEL.orEmpty().trim().ifBlank { "Android" },
        appVersion = BuildConfig.VERSION_NAME,
        biosStatus = context.getString(if (biosValid) R.string.ps1ui_bios_ready else R.string.ps1ui_bios_missing),
        biosValid = biosValid,
    )
}

private fun pushStringsToNative(context: Context) {
    val keys = Array(PS1_STRING_BINDINGS.size) { i -> PS1_STRING_BINDINGS[i].first }
    val values = Array(PS1_STRING_BINDINGS.size) { i ->
        runCatching { context.getString(PS1_STRING_BINDINGS[i].second) }.getOrDefault("")
    }
    Ps1ShellNative.nativeSetStrings(keys, values)
}

private fun pushGamesToNative(games: List<Ps1ResolvedGame>) {
    Ps1ShellNative.nativeSetGames(
        titles = games.map { it.title }.toTypedArray(),
        paths = games.map { it.path }.toTypedArray(),
        serials = games.map { it.serial }.toTypedArray(),
        covers = games.map { it.coverPath }.toTypedArray(),
        sizes = games.map { it.sizeStr }.toTypedArray(),
        dates = games.map { it.dateStr }.toTypedArray(),
        saveCounts = games.map { it.saveCount }.toIntArray(),
        latestSaveSlots = games.map { it.latestSaveSlot }.toIntArray(),
        discCounts = games.map { it.discCount }.toIntArray(),
        discLabelsPacked = games.map { it.discLabelsPacked }.toTypedArray(),
        discPathsPacked = games.map { it.discPathsPacked }.toTypedArray(),
    )
}

private fun pushSavesToNative(
    context: Context,
    saves: List<SaveStateEntryInfo>,
) {
    val autoLabel = context.getString(R.string.ps1ui_slot_auto)
    val titles = Array(saves.size) { i -> saves[i].gameTitle.ifBlank { saves[i].serial } }
    val slots = Array(saves.size) { i ->
        val s = saves[i]
        if (s.isAutoSave) autoLabel else context.getString(R.string.ps1ui_slot_fmt, s.slot)
    }
    val slotNums = IntArray(saves.size) { i -> saves[i].slot }
    val dates = Array(saves.size) { i -> formatTimestamp(saves[i].lastModified) }
    val sizes = Array(saves.size) { i -> formatFileSize(saves[i].sizeBytes) }
    val serials = Array(saves.size) { i -> saves[i].serial }
    val previews = Array(saves.size) { i -> saves[i].coverArtPath.orEmpty() }
    val savePaths = Array(saves.size) { i -> saves[i].absolutePath }
    val gamePaths = Array(saves.size) { i -> saves[i].gamePath.orEmpty() }
    Ps1ShellNative.nativeSetSaves(titles, slots, slotNums, dates, sizes, serials, previews, savePaths, gamePaths)
}

private fun pushSystemSnapshotToNative(snap: Ps1SystemSnapshot) {
    Ps1ShellNative.nativeSetSystemStatus(
        batteryLevel = snap.batteryLevel,
        batteryCharging = snap.batteryCharging,
        wifiConnected = snap.wifiConnected,
        bluetoothConnected = snap.bluetoothConnected,
        audioMuted = snap.audioMuted,
        freeStorage = snap.freeStorage,
        deviceName = snap.deviceName,
        appVersion = snap.appVersion,
        biosStatus = snap.biosStatus,
        biosValid = snap.biosValid,
    )
}

private fun pushMemoryCardsToNative(context: Context, bundle: Ps1MemoryCardBundle) {
    val cardDateFmt = SimpleDateFormat("dd/MM/yyyy", Locale.getDefault())
    Ps1ShellNative.nativeSetMemoryCards(
        slot1Name = bundle.slot1Name,
        slot1Size = bundle.slot1Size,
        slot1Status = bundle.slot1Status,
        slot2Name = bundle.slot2Name,
        slot2Size = bundle.slot2Size,
        slot2Status = bundle.slot2Status,
        cardNames = bundle.cards.map { it.name }.toTypedArray(),
        cardSizes = bundle.cards.map { formatFileSize(it.sizeBytes) }.toTypedArray(),
        cardDates = bundle.cards.map { cardDateFmt.format(Date(it.modifiedTime)) }.toTypedArray(),
    )
}

/**
 * Full-screen native GLES 3.0 PlayStation 1 BIOS UI replacing EmuCoreR's 3D carousel shelf.
 */
@SuppressLint("ClickableViewAccessibility")
@Composable
fun Ps1ShellScreen(
    games: List<GameItem>,
    isLoadingGames: Boolean,
    isCoverArtDisabled: Boolean,
    onGamePathSelected: (String) -> Unit,
    onSaveSelected: (gamePath: String, slot: Int) -> Unit,
    onManageGame: (GameItem) -> Unit,
    onExitShell: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val coroutineScope = rememberCoroutineScope()
    val localeTag = context.resources.configuration.locales.toLanguageTags()

    val currentGames = rememberUpdatedState(games)
    val currentLoadingGames = rememberUpdatedState(isLoadingGames)
    val currentGameLaunch = rememberUpdatedState(onGamePathSelected)
    val currentSaveLaunch = rememberUpdatedState(onSaveSelected)
    val currentManageGame = rememberUpdatedState(onManageGame)
    val currentExit = rememberUpdatedState(onExitShell)

    val appPreferences = remember(context) { AppPreferences(context) }
    val customCoverRepo = remember(context) { CustomGameCoverRepository(context) }
    val coverArtRepo = remember(context) { CoverArtRepository(context) }
    val saveRepo = remember(context) { SaveStateRepository(context) }
    val cardRepo = remember(context) { MemoryCardRepository(context, appPreferences) }

    val shellPrefs = remember(context) {
        context.getSharedPreferences("ps1_shell_state", Context.MODE_PRIVATE)
    }

    var savesRevision by remember { mutableIntStateOf(0) }
    val saveStates by produceState<List<SaveStateEntryInfo>>(
        initialValue = Ps1ShellSessionCache.lastSaves,
        savesRevision,
        games,
        context
    ) {
        val enriched = withContext(Dispatchers.IO) {
            val raw = runCatching { saveRepo.listEntries() }.getOrDefault(emptyList())
            val firstPass = runCatching { saveRepo.enrichGlobalEntries(raw) }.getOrDefault(raw)
            // Also resolve multi-disc & local library paths cleanly by serial if missing
            val bySerial = games.mapNotNull { g ->
                normalizeSerialKey(g.serial)?.let { it to g }
            }.toMap()
            firstPass.map { entry ->
                val match = if (entry.gamePath.isNullOrBlank()) {
                    normalizeSerialKey(entry.serial)?.let { bySerial[it] }
                } else {
                    null
                }
                val base = if (match != null) {
                    entry.copy(
                        gamePath = match.path,
                        gameTitle = match.title,
                        coverArtPath = match.coverArtPath ?: entry.coverArtPath
                    )
                } else {
                    entry
                }
                // Resolve preview image file existence on Dispatchers.IO so the GL thread never stats files.
                val repoPreview = runCatching { saveRepo.getPreviewImagePath(base) }.getOrNull()
                val sidecar = File("${base.absolutePath}.png")
                val resolvedPreview = when {
                    !repoPreview.isNullOrBlank() && File(repoPreview).isFile -> repoPreview
                    sidecar.isFile && sidecar.length() > 0L -> sidecar.absolutePath
                    !base.coverArtPath.isNullOrBlank() && File(base.coverArtPath).isFile -> base.coverArtPath
                    else -> ""
                }
                base.copy(coverArtPath = resolvedPreview)
            }
        }
        Ps1ShellSessionCache.lastSaves = enriched
        Ps1ShellSessionCache.hasLoadedSaves = true
        value = enriched
    }
    val currentSaves = rememberUpdatedState(saveStates)

    val resolvedGames by produceState<List<Ps1ResolvedGame>>(
        initialValue = emptyList(),
        games,
        saveStates,
        isCoverArtDisabled,
        context
    ) {
        value = withContext(Dispatchers.IO) {
            val savesByGamePath = saveStates.groupBy { it.gamePath.orEmpty() }
            val savesBySerial = saveStates.mapNotNull { sv ->
                normalizeSerialKey(sv.serial)?.let { it to sv }
            }.groupBy({ it.first }, { it.second })

            games.map { game ->
                val customCover = customCoverRepo.findCustomCoverPath(game.path)
                val cachedCover = if (!isCoverArtDisabled) {
                    game.coverArtPath?.takeIf { File(it).isFile }
                        ?: coverArtRepo.findCachedCoverPath(game.serial)
                } else {
                    null
                }
                val resolvedCover = customCover ?: cachedCover.orEmpty()

                val discPaths = if (game.discs.size > 1) {
                    game.discs.map { it.game.path }
                } else {
                    listOf(game.path)
                }
                val discLabels = if (game.discs.size > 1) {
                    game.discs.map { d ->
                        context.getString(R.string.ps1ui_disc) + " " + d.number
                    }
                } else {
                    emptyList()
                }
                val matchingSaves = buildList {
                    discPaths.forEach { dp -> addAll(savesByGamePath[dp].orEmpty()) }
                    normalizeSerialKey(game.serial)?.let { ser ->
                        addAll(savesBySerial[ser].orEmpty())
                    }
                }.distinctBy { it.absolutePath }
                val latestSlot = matchingSaves.maxByOrNull { it.lastModified }?.slot ?: -1

                Ps1ResolvedGame(
                    title = game.title,
                    path = game.path,
                    serial = game.serial.orEmpty(),
                    coverPath = resolvedCover,
                    sizeStr = formatFileSize(game.fileSize),
                    dateStr = formatTimestamp(game.lastModified),
                    saveCount = matchingSaves.size,
                    latestSaveSlot = latestSlot,
                    discCount = discPaths.size.coerceAtLeast(1),
                    discLabelsPacked = discLabels.joinToString("\n"),
                    discPathsPacked = discPaths.joinToString("\n"),
                )
            }
        }
    }
    val currentResolvedGames = rememberUpdatedState(resolvedGames)

    val memoryCardBundle by produceState(initialValue = Ps1MemoryCardBundle(), savesRevision, context) {
        value = withContext(Dispatchers.IO) {
            val assignments = runCatching { cardRepo.currentAssignments() }.getOrNull()
            val cards = runCatching { cardRepo.listCards() }.getOrDefault(emptyList())
            val readyStr = context.getString(R.string.ps1ui_card_formatted)
            val s1 = assignments?.slot1?.let { name -> cards.find { it.name.equals(name, ignoreCase = true) } }
            val s2 = assignments?.slot2?.let { name -> cards.find { it.name.equals(name, ignoreCase = true) } }
            Ps1MemoryCardBundle(
                slot1Name = assignments?.slot1.orEmpty(),
                slot1Size = s1?.let { formatFileSize(it.sizeBytes) } ?: "",
                slot1Status = if (assignments?.slot1 != null) readyStr else "",
                slot2Name = assignments?.slot2.orEmpty(),
                slot2Size = s2?.let { formatFileSize(it.sizeBytes) } ?: "",
                slot2Status = if (assignments?.slot2 != null) readyStr else "",
                cards = cards,
            )
        }
    }
    val currentCardBundle = rememberUpdatedState(memoryCardBundle)

    val biosValidState by produceState(initialValue = Ps1ShellSessionCache.lastBiosValid, context) {
        value = withContext(Dispatchers.IO) {
            val path = appPreferences.biosPath.first()
            BiosValidator.hasUsableBiosFiles(context, path).also {
                Ps1ShellSessionCache.lastBiosValid = it
            }
        }
    }
    val currentBiosValid = rememberUpdatedState(biosValidState)

    fun applyHostSettings(
        keyTone: Boolean,
        ambientSound: Boolean,
        swapConfirm: Boolean,
        keepScreenOn: Boolean,
    ) {
        Ps1ShellAudio.keyToneEnabled = keyTone
        Ps1ShellAudio.ambientEnabled = ambientSound
        Ps1ShellHost.swapConfirmButton = swapConfirm
        val window = (context as? Activity)?.window
        if (keepScreenOn) {
            window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    fun pushSettingsToNativeOnGlThread() {
        val theme = shellPrefs.getInt("theme", 0)
        val aspectMode = shellPrefs.getInt("aspect_mode", 0)
        val orbsMode = shellPrefs.getInt("orbs_mode", 0)
        val scanlinesMode = shellPrefs.getInt("scanlines_mode", 0)
        val bootAnimMode = shellPrefs.getInt("boot_anim_mode", 0)
        val keyTone = shellPrefs.getBoolean("key_tone", true)
        val ambientSound = shellPrefs.getBoolean("ambient_sound", false)
        val swapConfirm = shellPrefs.getBoolean("swap_confirm", false)
        val touchControls = shellPrefs.getBoolean("touch_controls", true)
        val time24h = shellPrefs.getBoolean("time_24h", true)
        val dateFormat = shellPrefs.getInt("date_format", 0)
        val keepScreenOn = shellPrefs.getBoolean("keep_screen_on", true)
        Ps1ShellNative.nativeSetSettings(
            theme = theme,
            aspectMode = aspectMode,
            orbsMode = orbsMode,
            scanlinesMode = scanlinesMode,
            bootAnimMode = bootAnimMode,
            keyTone = keyTone,
            ambientSound = ambientSound,
            swapConfirm = swapConfirm,
            touchControls = touchControls,
            time24h = time24h,
            dateFormat = dateFormat,
            keepScreenOn = keepScreenOn,
        )
    }

    fun pushSystemStatusOnGlThread() {
        pushSystemSnapshotToNative(Ps1ShellSessionCache.lastSystemSnapshot)
    }

    fun refreshSystemStatusAsync(targetSurface: GLSurfaceView) {
        coroutineScope.launch(Dispatchers.IO) {
            val snap = readSystemSnapshot(context, currentBiosValid.value)
            Ps1ShellSessionCache.lastSystemSnapshot = snap
            targetSurface.queueEvent { pushSystemSnapshotToNative(snap) }
        }
    }

    fun syncTouchModeOnGlThread() {
        val touchEnabled = shellPrefs.getBoolean("touch_controls", true)
        val touchActive = touchEnabled && !hasPhysicalGamepadConnected()
        val tutorialShown = shellPrefs.getBoolean("touch_tutorial_shown", false)
        val showGuide = touchActive && !tutorialShown
        if (showGuide) {
            shellPrefs.edit().putBoolean("touch_tutorial_shown", true).apply()
        }
        Ps1ShellNative.nativeSetTouchMode(touchActive, showGuide)
    }

    fun saveShellNavigationState() {
        val state = Ps1ShellNative.nativeGetState()
        if (state.size >= 4) {
            shellPrefs.edit()
                .putInt("screen", state[0])
                .putInt("main_index", state[1])
                .putInt("game_index", state[2])
                .putInt("save_index", state[3])
                .apply()
        }
    }

    val mainActivity = context as? MainActivity
    val orientationToken = remember { "ps1shell_${System.identityHashCode(Any())}" }

    val surface = remember(context) {
        var downX = 0f
        var downY = 0f
        var multiTouchHandled = false
        var longPressTriggered = false
        var longPressRunnable: Runnable? = null

        GLSurfaceView(context).apply {
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(
                Ps1ShellSurfaceRenderer(context) {
                    pushStringsToNative(context)
                    pushSettingsToNativeOnGlThread()
                    pushSystemStatusOnGlThread()
                    syncTouchModeOnGlThread()

                    val gamesSnap = currentResolvedGames.value
                    val savesSnap = currentSaves.value
                    Ps1ShellNative.nativeSetGamesLoading(currentLoadingGames.value && gamesSnap.isEmpty())
                    Ps1ShellNative.nativeSetSavesLoading(!Ps1ShellSessionCache.hasLoadedSaves && savesSnap.isEmpty())
                    pushGamesToNative(gamesSnap)
                    pushSavesToNative(context, savesSnap)
                    pushMemoryCardsToNative(context, currentCardBundle.value)

                    Ps1ShellNative.nativeSetState(
                        shellPrefs.getInt("screen", 0),
                        shellPrefs.getInt("main_index", 0),
                        shellPrefs.getInt("game_index", 0),
                        shellPrefs.getInt("save_index", 0),
                    )
                    if (Ps1ShellSessionCache.returnedFromGame) {
                        Ps1ShellSessionCache.returnedFromGame = false
                        Ps1ShellNative.nativeTriggerTransition(1)
                    } else {
                        Ps1ShellNative.nativeTriggerTransition(0)
                    }
                }
            )
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            isFocusable = true
            isFocusableInTouchMode = true
            defaultFocusHighlightEnabled = false
            requestFocus()

            setOnKeyListener { _, _, event -> Ps1ShellHost.handleKeyEvent(event) }

            setOnTouchListener { v, event ->
                val touchEnabled = shellPrefs.getBoolean("touch_controls", true)
                if (!touchEnabled && event.pointerCount < 2) {
                    return@setOnTouchListener true
                }
                val w = v.width.coerceAtLeast(1).toFloat()
                val h = v.height.coerceAtLeast(1).toFloat()
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        downX = event.x
                        downY = event.y
                        multiTouchHandled = false
                        longPressTriggered = false
                        val nx = event.x / w
                        val ny = event.y / h
                        longPressRunnable?.let(v::removeCallbacks)
                        val lp = Runnable {
                            if (!multiTouchHandled) {
                                longPressTriggered = true
                                queueEvent { Ps1ShellNative.nativeTouchLongPress(nx, ny) }
                            }
                        }
                        longPressRunnable = lp
                        v.postDelayed(lp, 440L)
                        queueEvent { Ps1ShellNative.nativeTouchDown(nx, ny) }
                        true
                    }
                    MotionEvent.ACTION_POINTER_DOWN -> {
                        if (event.pointerCount >= 2 && !multiTouchHandled) {
                            longPressRunnable?.let(v::removeCallbacks)
                            multiTouchHandled = true
                            queueEvent { Ps1ShellNative.nativeTouchTwoFingerBack() }
                        }
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        if (abs(event.x - downX) > 28f || abs(event.y - downY) > 28f) {
                            longPressRunnable?.let(v::removeCallbacks)
                        }
                        if (!multiTouchHandled && !longPressTriggered) {
                            val nx = event.x / w
                            val ny = event.y / h
                            queueEvent { Ps1ShellNative.nativeTouchMove(nx, ny) }
                        }
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        longPressRunnable?.let(v::removeCallbacks)
                        if (!multiTouchHandled && !longPressTriggered) {
                            val nx = event.x / w
                            val ny = event.y / h
                            queueEvent { Ps1ShellNative.nativeTouchUp(nx, ny) }
                        } else {
                            queueEvent { Ps1ShellNative.nativeTouchCancel() }
                        }
                        true
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        longPressRunnable?.let(v::removeCallbacks)
                        queueEvent { Ps1ShellNative.nativeTouchCancel() }
                        true
                    }
                    else -> true
                }
            }
        }
    }

    BackHandler {
        surface.queueEvent {
            Ps1ShellNative.nativeKey(Ps1ShellHost.KEY_BACK)
        }
    }

    DisposableEffect(mainActivity, orientationToken) {
        mainActivity?.setOrientationOverride(
            orientationToken,
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        )
        onDispose {
            mainActivity?.clearOrientationOverride(orientationToken)
        }
    }

    val statusReceiver = remember {
        object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                refreshSystemStatusAsync(surface)
            }
        }
    }

    DisposableEffect(surface, lifecycleOwner) {
        UiSoundManager.setNativeShellActive(true)
        Ps1ShellAudio.resumeAudio()

        applyHostSettings(
            keyTone = shellPrefs.getBoolean("key_tone", true),
            ambientSound = shellPrefs.getBoolean("ambient_sound", false),
            swapConfirm = shellPrefs.getBoolean("swap_confirm", false),
            keepScreenOn = shellPrefs.getBoolean("keep_screen_on", true),
        )

        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_BATTERY_CHANGED)
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED)
            @Suppress("DEPRECATION")
            addAction(ConnectivityManager.CONNECTIVITY_ACTION)
            addAction("android.media.VOLUME_CHANGED_ACTION")
        }
        runCatching { context.registerReceiver(statusReceiver, filter) }

        Ps1ShellHost.setGameLaunchListener { gamePath ->
            saveShellNavigationState()
            Ps1ShellSessionCache.returnedFromGame = true
            Ps1ShellAudio.pauseAudio()
            currentGameLaunch.value(gamePath)
        }

        Ps1ShellHost.setSaveLaunchListener { gamePath, slot ->
            saveShellNavigationState()
            Ps1ShellSessionCache.returnedFromGame = true
            Ps1ShellAudio.pauseAudio()
            currentSaveLaunch.value(gamePath, slot)
        }

        Ps1ShellHost.setDeleteSaveListener { index ->
            val entry = currentSaves.value.getOrNull(index)
            if (entry != null) {
                coroutineScope.launch(Dispatchers.IO) {
                    saveRepo.deleteEntry(entry)
                    Ps1ShellSessionCache.lastSaves = Ps1ShellSessionCache.lastSaves.filterNot {
                        it.absolutePath == entry.absolutePath
                    }
                    withContext(Dispatchers.Main) {
                        savesRevision++
                    }
                }
            }
        }

        Ps1ShellHost.setOpenGameSettingsListener { index ->
            val game = currentGames.value.getOrNull(index)
            if (game != null) {
                saveShellNavigationState()
                Ps1ShellSessionCache.returnedFromGame = true
                Ps1ShellAudio.pauseAudio()
                currentManageGame.value(game)
            }
        }

        Ps1ShellHost.setSettingsListener { arr ->
            if (arr.size >= 12) {
                val keyTone = arr[5] != 0
                val ambientSound = arr[6] != 0
                val swapConfirm = arr[7] != 0
                val touchControls = arr[8] != 0
                val time24h = arr[9] != 0
                val keepScreenOn = arr[11] != 0
                shellPrefs.edit()
                    .putInt("theme", arr[0])
                    .putInt("aspect_mode", arr[1])
                    .putInt("orbs_mode", arr[2])
                    .putInt("scanlines_mode", arr[3])
                    .putInt("boot_anim_mode", arr[4])
                    .putBoolean("key_tone", keyTone)
                    .putBoolean("ambient_sound", ambientSound)
                    .putBoolean("swap_confirm", swapConfirm)
                    .putBoolean("touch_controls", touchControls)
                    .putBoolean("time_24h", time24h)
                    .putInt("date_format", arr[10])
                    .putBoolean("keep_screen_on", keepScreenOn)
                    .apply()
                applyHostSettings(keyTone, ambientSound, swapConfirm, keepScreenOn)
                surface.queueEvent { syncTouchModeOnGlThread() }
            }
        }

        Ps1ShellHost.setExitListener {
            saveShellNavigationState()
            Ps1ShellAudio.pauseAudio()
            UiSoundManager.setNativeShellActive(false)
            mainActivity?.clearOrientationOverride(orientationToken)
            currentExit.value()
        }

        Ps1ShellHost.setKeyListener { code ->
            surface.queueEvent {
                Ps1ShellNative.nativeKey(code)
            }
        }

        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    mainActivity?.setOrientationOverride(
                        orientationToken,
                        ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                    )
                    surface.onResume()
                    surface.requestFocus()
                    Ps1ShellAudio.resumeAudio()
                    savesRevision++
                    refreshSystemStatusAsync(surface)
                    surface.queueEvent {
                        syncTouchModeOnGlThread()
                    }
                }
                Lifecycle.Event.ON_PAUSE -> {
                    saveShellNavigationState()
                    Ps1ShellAudio.pauseAudio()
                    surface.onPause()
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)

        onDispose {
            saveShellNavigationState()
            Ps1ShellAudio.pauseAudio()
            UiSoundManager.setNativeShellActive(false)
            (context as? Activity)?.window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            runCatching { context.unregisterReceiver(statusReceiver) }
            lifecycleOwner.lifecycle.removeObserver(observer)
            Ps1ShellHost.setGameLaunchListener(null)
            Ps1ShellHost.setSaveLaunchListener(null)
            Ps1ShellHost.setDeleteSaveListener(null)
            Ps1ShellHost.setOpenGameSettingsListener(null)
            Ps1ShellHost.setSettingsListener(null)
            Ps1ShellHost.setExitListener(null)
            Ps1ShellHost.setKeyListener(null)
        }
    }

    LaunchedEffect(localeTag) {
        val saves = currentSaves.value
        val cards = currentCardBundle.value
        refreshSystemStatusAsync(surface)
        surface.queueEvent {
            pushStringsToNative(context)
            pushSavesToNative(context, saves)
            pushMemoryCardsToNative(context, cards)
        }
    }

    LaunchedEffect(isLoadingGames, resolvedGames) {
        val snap = resolvedGames
        val loading = isLoadingGames && snap.isEmpty()
        surface.queueEvent {
            Ps1ShellNative.nativeSetGamesLoading(loading)
            pushGamesToNative(snap)
        }
    }

    LaunchedEffect(saveStates) {
        val saves = saveStates
        val loading = !Ps1ShellSessionCache.hasLoadedSaves && saves.isEmpty()
        surface.queueEvent {
            Ps1ShellNative.nativeSetSavesLoading(loading)
            pushSavesToNative(context, saves)
        }
    }

    LaunchedEffect(memoryCardBundle) {
        val bundle = memoryCardBundle
        surface.queueEvent {
            pushMemoryCardsToNative(context, bundle)
        }
    }

    LaunchedEffect(biosValidState) {
        refreshSystemStatusAsync(surface)
    }

    AndroidView(
        factory = { surface },
        modifier = modifier,
    )
}
