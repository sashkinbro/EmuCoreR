package com.sbro.emucorer.data

import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.drawable.Animatable
import android.graphics.drawable.BitmapDrawable
import android.os.Build
import android.util.Base64
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class HomeBackgroundDecoderInstrumentedTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun legacyDecoderSamplesLargeImagesBeforeAllocatingTheDisplayedBitmap() {
        withImageFile(4096, 32) { file ->
            val drawable = decodeLegacyHomeBackgroundDrawable(context.resources, file, 2048)
            try {
                assertEquals(2048, drawable.bitmap.width)
                assertEquals(16, drawable.bitmap.height)
            } finally {
                drawable.bitmap.recycle()
            }
        }
    }

    @Test
    fun legacyDecoderRejectsCorruptContent() {
        val file = File.createTempFile("background-corrupt", ".image", context.cacheDir)
        try {
            file.writeText("This is not an image")
            assertThrows(IllegalArgumentException::class.java) {
                decodeLegacyHomeBackgroundDrawable(context.resources, file, 2048)
            }
        } finally {
            file.delete()
        }
    }

    @Test
    fun platformDecoderDisplaysSmallImagesAtTheirOriginalSize() {
        withImageFile(24, 16) { file ->
            val drawable = decodeHomeBackgroundDrawable(context.resources, file, 2048)
            assertEquals(24, drawable.intrinsicWidth)
            assertEquals(16, drawable.intrinsicHeight)
            (drawable as? BitmapDrawable)?.bitmap?.recycle()
        }
    }

    @Test
    fun legacyDecoderDisplaysTheFirstFrameOfAnAnimatedGif() {
        withAnimatedGif { file ->
            val drawable = decodeLegacyHomeBackgroundDrawable(context.resources, file, 2048)
            try {
                assertEquals(Color.RED, drawable.bitmap.getPixel(0, 0))
            } finally {
                drawable.bitmap.recycle()
            }
        }
    }

    @Test
    fun platformDecoderPreservesGifAnimationOnSupportedAndroidVersions() {
        withAnimatedGif { file ->
            val drawable = decodeHomeBackgroundDrawable(context.resources, file, 2048)
            assertEquals(1, drawable.intrinsicWidth)
            assertEquals(1, drawable.intrinsicHeight)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                assertTrue(drawable is Animatable)
            } else {
                assertTrue(drawable is BitmapDrawable)
            }
        }
    }

    private fun withAnimatedGif(block: (File) -> Unit) {
        val file = File.createTempFile("background-animation", ".gif", context.cacheDir)
        try {
            // Two 1x1 frames, red then blue; each frame lasts 10 ms.
            file.writeBytes(Base64.decode(
                "R0lGODlhAQABAIAAAP8AAAAA/yH5BAABAAAALAAAAAABAAEAAAICRAEAIfkEAAEAAAAsAAAAAAEAAQAAAgJMAQA7",
                Base64.DEFAULT
            ))
            block(file)
        } finally {
            file.delete()
        }
    }

    private fun withImageFile(width: Int, height: Int, block: (File) -> Unit) {
        val file = File.createTempFile("background-image", ".image", context.cacheDir)
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        try {
            file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
            block(file)
        } finally {
            bitmap.recycle()
            file.delete()
        }
    }
}
