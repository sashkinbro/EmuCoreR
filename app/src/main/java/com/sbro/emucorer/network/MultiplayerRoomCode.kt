// SPDX-FileCopyrightText: 2026 EmuCoreR contributors
// SPDX-License-Identifier: GPL-3.0+

package com.sbro.emucorer.network

import java.util.Locale

internal const val MULTIPLAYER_ROOM_CODE_LENGTH = 8
internal const val MULTIPLAYER_ROOM_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

internal fun sanitizeMultiplayerRoomCode(value: String): String = value
    .uppercase(Locale.ROOT)
    .filter { it in MULTIPLAYER_ROOM_ALPHABET }
    .take(MULTIPLAYER_ROOM_CODE_LENGTH)
