package com.sbro.emucorer.ui.emulation

import com.sbro.emucorer.data.PerformanceOverlayMetrics

internal data class PerformanceOverlayLayout(
    val mainLines: List<String>,
    val bottomLines: List<String>
)

private val overlayColonSpacing = Regex(":\\s+")
private val overlayPipeSpacing = Regex("\\s*\\|\\s*")
private val overlaySlashSpacing = Regex("\\s*/\\s*")
private val overlayGroupingSpacing = Regex("\\s+(?=[\\[(])")
private val overlayUnitSpacing = Regex("(?<=\\d)\\s+(?=(?:ms|MB)\\b)")

internal fun compactPerformanceOverlayLine(line: String): String {
    return line
        .replace(overlayColonSpacing, ":")
        .replace(overlayPipeSpacing, "|")
        .replace(overlaySlashSpacing, "/")
        .replace(overlayGroupingSpacing, "")
        .replace(overlayUnitSpacing, "")
        .trim()
}

internal fun buildPerformanceOverlayLayout(
    text: String,
    metricsMask: Int,
    fixedHeaderLine: String = ""
): PerformanceOverlayLayout {
    fun isRendererLine(line: String): Boolean {
        return line.endsWith(" HW") ||
            line.endsWith(" SW") ||
            line.endsWith(" Null") ||
            line.contains(" HW |") ||
            line.contains(" SW |") ||
            line.contains(" Null |")
    }

    fun metricForSegment(segment: String): Int = when {
        segment.startsWith("FPS:") -> PerformanceOverlayMetrics.FPS
        segment.startsWith("Speed:") -> PerformanceOverlayMetrics.SPEED
        segment.startsWith("Target:") -> PerformanceOverlayMetrics.TARGET
        else -> 0
    }

    fun filterLine(line: String): String? {
        if (line.startsWith("FPS:") ||
            line.startsWith("Speed:") || line.startsWith("Target:")
        ) {
            return line.split(" | ")
                .filter { segment ->
                    val metric = metricForSegment(segment)
                    metric == 0 || PerformanceOverlayMetrics.isEnabled(metricsMask, metric)
                }
                .joinToString(" | ")
                .ifBlank { null }
        }

        val metric = when {
            isRendererLine(line) -> PerformanceOverlayMetrics.RENDERER
            line.startsWith("Frame:") -> PerformanceOverlayMetrics.FRAME_TIME
            line.startsWith("Res:") -> PerformanceOverlayMetrics.RESOLUTION
            line.startsWith("CPU:") -> PerformanceOverlayMetrics.HOST_CPU
            line.startsWith("GPU:") -> PerformanceOverlayMetrics.HOST_GPU
            line.startsWith("Audio:") -> PerformanceOverlayMetrics.AUDIO
            else -> 0
        }
        if (metric != 0 && metric != PerformanceOverlayMetrics.HOST_GPU &&
            !PerformanceOverlayMetrics.isEnabled(metricsMask, metric)
        ) {
            return null
        }
        // The host GPU is shown alongside the host CPU: selecting CPU also
        // enables the GPU line so the pair is never split.
        if (metric == PerformanceOverlayMetrics.HOST_GPU &&
            !PerformanceOverlayMetrics.isEnabled(metricsMask, PerformanceOverlayMetrics.HOST_CPU)
        ) {
            return null
        }
        return line
    }

    val filtered = text.lineSequence()
        .map { it.trim() }
        .filter { it.isNotEmpty() }
        .mapNotNull(::filterLine)
        .toList()

    val topLines = filtered.filter { line ->
        line.startsWith("FPS:") ||
            line.startsWith("Speed:") || line.startsWith("Target:")
    }
    val hardwareLines = filtered.filter { line ->
        line.startsWith("CPU:") || line.startsWith("GPU:")
    }
    val audioLines = filtered.filter { line -> line.startsWith("Audio:") }
    val rendererLine = filtered.firstOrNull(::isRendererLine)
    val bottomLines = buildList {
        rendererLine?.let(::add)
        addAll(filtered.filter { line ->
            line.startsWith("Frame:") || line.startsWith("Res:")
        })
    }
    val knownLines = (topLines + hardwareLines + audioLines + bottomLines).toSet()
    val unknownLines = filtered.filterNot { line ->
        line in knownLines || line == rendererLine
    }

    return PerformanceOverlayLayout(
        mainLines = (
            listOf(fixedHeaderLine).filter(String::isNotBlank) +
                topLines + hardwareLines + audioLines + unknownLines
            ).map(::compactPerformanceOverlayLine),
        bottomLines = bottomLines.map(::compactPerformanceOverlayLine)
    )
}
