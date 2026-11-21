package com.sbro.emucorer.ps1ui

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import android.text.TextUtils
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * High-DPI shaped text rasterizer for the native GLES 3.0 PS1 UI.
 * Uses Android's StaticLayout + TextPaint so all 18 languages (including RTL
 * Arabic/Persian, Devanagari Hindi, CJK, Cyrillic, and Latin) render crisply
 * at native screen density with authentic PS1 drop shadows.
 */
object Ps1TextRasterizer {
    const val STYLE_HEADER = 0
    const val STYLE_GRAFFITI = 1
    const val STYLE_BODY = 2
    const val STYLE_BODY_BOLD = 3
    const val STYLE_MONO = 4

    const val ALIGN_START = 0
    const val ALIGN_CENTER = 1
    const val ALIGN_END = 2

    private val lock = Any()

    private val condensedBold: Typeface =
        Typeface.create("sans-serif-condensed", Typeface.BOLD)
    private val sansRegular: Typeface =
        Typeface.create("sans-serif", Typeface.NORMAL)
    private val sansBold: Typeface =
        Typeface.create("sans-serif", Typeface.BOLD)
    private val monoBold: Typeface =
        Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)

    private val mainPaint = TextPaint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG).apply {
        color = Color.WHITE
    }
    private val shadowPaint = TextPaint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG)
    private val outlinePaint = TextPaint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG)

    /**
     * Rasterizes [text] into a byte buffer:
     * `[pixelWidth, pixelHeight, logicalWidthX100, logicalHeightX100]` (16 bytes LE)
     * followed by `pixelWidth * pixelHeight * 4` straight RGBA8888 bytes.
     */
    @JvmStatic
    fun rasterizeText(
        text: String,
        virtualFontSize: Float,
        style: Int,
        maxVirtualWidth: Float,
        maxLines: Int,
        align: Int,
        rasterScale: Float,
    ): ByteArray? {
        if (text.isEmpty()) return null
        val scale = rasterScale.coerceIn(1.25f, 3.75f)
        val pxFontSize = (virtualFontSize * scale).coerceIn(10f, 220f)
        val clampedMaxLines = maxLines.coerceIn(1, 16)
        val hasMaxWidth = maxVirtualWidth > 1f
        val pxMaxWidth = if (hasMaxWidth) {
            (maxVirtualWidth * scale).roundToInt().coerceIn(8, 3072)
        } else {
            3072
        }

        synchronized(lock) {
            val typeface = when (style) {
                STYLE_HEADER, STYLE_GRAFFITI -> condensedBold
                STYLE_BODY_BOLD -> sansBold
                STYLE_MONO -> monoBold
                else -> sansRegular
            }
            mainPaint.typeface = typeface
            mainPaint.textSize = pxFontSize
            mainPaint.letterSpacing = when (style) {
                STYLE_GRAFFITI -> 0.04f
                STYLE_HEADER -> 0.05f
                else -> 0.0f
            }

            val shadowOffset = when (style) {
                STYLE_GRAFFITI -> max(2.2f, pxFontSize * 0.075f)
                STYLE_HEADER -> max(1.8f, pxFontSize * 0.07f)
                else -> max(1.0f, pxFontSize * 0.05f)
            }
            val padRight = ceil(shadowOffset + if (style == STYLE_GRAFFITI) scale * 2.5f else scale * 1.5f).toInt()
            val padLeft = ceil(if (style == STYLE_GRAFFITI) scale * 2.0f else scale * 1.0f).toInt()
            val padTop = ceil(scale * 1.2f).toInt()
            val padBottom = ceil(shadowOffset + scale * 1.5f).toInt()

            val effectiveAvailPx = if (hasMaxWidth) {
                (pxMaxWidth - padLeft - padRight).coerceAtLeast(8)
            } else {
                ceil(StaticLayout.getDesiredWidth(text, mainPaint)).toInt().coerceIn(4, 3000)
            }

            val layoutAlign = when (align) {
                ALIGN_CENTER -> Layout.Alignment.ALIGN_CENTER
                ALIGN_END -> Layout.Alignment.ALIGN_OPPOSITE
                else -> Layout.Alignment.ALIGN_NORMAL
            }

            val layoutWidth = if (hasMaxWidth && align != ALIGN_START) {
                effectiveAvailPx
            } else {
                val desired = ceil(StaticLayout.getDesiredWidth(text, mainPaint)).toInt().coerceAtLeast(1)
                if (hasMaxWidth) desired.coerceAtMost(effectiveAvailPx) else desired
            }

            val mainLayout = buildLayout(text, mainPaint, layoutWidth, layoutAlign, clampedMaxLines, effectiveAvailPx)
            var measuredContentW = 0f
            for (i in 0 until mainLayout.lineCount) {
                measuredContentW = max(measuredContentW, mainLayout.getLineWidth(i))
            }
            val finalLayoutWidth = if (hasMaxWidth && align != ALIGN_START) {
                effectiveAvailPx
            } else {
                ceil(measuredContentW).toInt().coerceIn(1, effectiveAvailPx)
            }
            val finalMainLayout = if (finalLayoutWidth != layoutWidth) {
                buildLayout(text, mainPaint, finalLayoutWidth, layoutAlign, clampedMaxLines, finalLayoutWidth)
            } else {
                mainLayout
            }

            val texW = (finalMainLayout.width + padLeft + padRight).coerceIn(2, 3200)
            val texH = (finalMainLayout.height + padTop + padBottom).coerceIn(2, 2048)

            val bitmap = Bitmap.createBitmap(texW, texH, Bitmap.Config.ARGB_8888)
            try {
                val canvas = Canvas(bitmap)

                // Hard PS1-style drop shadow
                shadowPaint.set(mainPaint)
                shadowPaint.color = when (style) {
                    STYLE_GRAFFITI -> Color.argb(245, 0, 0, 0)
                    STYLE_HEADER -> Color.argb(230, 0, 0, 8)
                    else -> Color.argb(190, 0, 0, 10)
                }
                val shadowLayout = buildLayout(
                    text, shadowPaint, finalLayoutWidth, layoutAlign, clampedMaxLines, finalLayoutWidth
                )
                canvas.save()
                canvas.translate(padLeft + shadowOffset, padTop + shadowOffset)
                shadowLayout.draw(canvas)
                canvas.restore()

                // Thin dark halo stroke behind main menu graffiti labels for contrast on bright splashes
                if (style == STYLE_GRAFFITI) {
                    outlinePaint.set(mainPaint)
                    outlinePaint.style = Paint.Style.STROKE
                    outlinePaint.strokeWidth = max(1.8f, pxFontSize * 0.06f)
                    outlinePaint.strokeJoin = Paint.Join.ROUND
                    outlinePaint.color = Color.argb(215, 4, 6, 22)
                    val strokeLayout = buildLayout(
                        text, outlinePaint, finalLayoutWidth, layoutAlign, clampedMaxLines, finalLayoutWidth
                    )
                    canvas.save()
                    canvas.translate(padLeft.toFloat(), padTop.toFloat())
                    strokeLayout.draw(canvas)
                    canvas.restore()
                }

                // Crisp foreground glyphs
                canvas.save()
                canvas.translate(padLeft.toFloat(), padTop.toFloat())
                finalMainLayout.draw(canvas)
                canvas.restore()

                val pixels = IntArray(texW * texH)
                bitmap.getPixels(pixels, 0, texW, 0, 0, texW, texH)

                val out = ByteBuffer.allocate(16 + texW * texH * 4).order(ByteOrder.LITTLE_ENDIAN)
                out.putInt(texW)
                out.putInt(texH)
                out.putInt(((texW / scale) * 100f).roundToInt())
                out.putInt(((texH / scale) * 100f).roundToInt())

                val rawBytes = out.array()
                var offset = 16
                for (argb in pixels) {
                    val a = (argb ushr 24) and 0xFF
                    if (a == 0) {
                        rawBytes[offset] = 0
                        rawBytes[offset + 1] = 0
                        rawBytes[offset + 2] = 0
                        rawBytes[offset + 3] = 0
                    } else {
                        val r = (argb ushr 16) and 0xFF
                        val g = (argb ushr 8) and 0xFF
                        val b = argb and 0xFF
                        rawBytes[offset] = r.toByte()
                        rawBytes[offset + 1] = g.toByte()
                        rawBytes[offset + 2] = b.toByte()
                        rawBytes[offset + 3] = a.toByte()
                    }
                    offset += 4
                }
                return rawBytes
            } finally {
                bitmap.recycle()
            }
        }
    }

    private fun buildLayout(
        text: String,
        paint: TextPaint,
        widthPx: Int,
        align: Layout.Alignment,
        maxLines: Int,
        ellipsizeWidthPx: Int,
    ): StaticLayout {
        return StaticLayout.Builder
            .obtain(text, 0, text.length, paint, widthPx.coerceAtLeast(1))
            .setAlignment(align)
            .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR)
            .setLineSpacing(0f, 1.08f)
            .setIncludePad(false)
            .setMaxLines(maxLines)
            .setEllipsize(TextUtils.TruncateAt.END)
            .setEllipsizedWidth(ellipsizeWidthPx.coerceAtLeast(1))
            .build()
    }
}
