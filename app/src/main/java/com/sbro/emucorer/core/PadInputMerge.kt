package com.sbro.emucorer.core

/**
 * Pure merge rules for the source-isolated pad state. Gamepads/touch/network
 * publish into the legacy slot, keyboard and mouse into their own slots.
 */
internal object PadInputMerge {
    const val ALL_RELEASED = 0xFFFF

    fun applyButton(mask: Int, bit: Int, pressed: Boolean): Int =
        if (pressed) mask and (1 shl bit).inv()
        else mask or (1 shl bit)

    fun effectiveButtons(
        legacy: Int,
        derivedDpad: Int,
        sourceMasks: Array<IntArray>,
        padIndex: Int
    ): Int {
        var buttons = legacy and derivedDpad
        for (mask in sourceMasks) {
            buttons = buttons and mask[padIndex]
        }
        return buttons
    }

    fun strongestHalfAxis(
        legacy: Int,
        sourceHalfAxes: Array<Array<IntArray>>,
        padIndex: Int,
        axis: Int
    ): Int {
        var strongest = legacy
        for (source in sourceHalfAxes) {
            strongest = maxOf(strongest, source[padIndex][axis])
        }
        return strongest
    }
}
