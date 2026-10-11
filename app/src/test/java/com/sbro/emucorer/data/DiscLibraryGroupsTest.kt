package com.sbro.emucorer.data

import org.junit.Assert.*
import org.junit.Test

class DiscLibraryGroupsTest {
    private fun disc(name: String, folder: String = "/games", size: Long = 100) =
        GameItem("Oddworld", "$folder/$name", name, size, size)

    @Test fun groupsAndOrdersDiscsWithoutChangingTheirLaunchPaths() {
        val second = disc("Oddworld (USA) (Disc 2).chd", size = 200)
        val first = disc("Oddworld (USA) (Disc 1).chd")
        val group = groupLibraryDiscs(listOf(second, first)).single()
        assertEquals(first.path, group.path)
        assertEquals(listOf(1, 2), group.discs.map { it.number })
        assertEquals(listOf(first, second), group.discs.map { it.game })
        assertEquals(300, group.fileSize)
        assertEquals(200, group.lastModified)
    }

    @Test fun keepsRegionsRevisionsAndFoldersSeparate() {
        val files = listOf(
            disc("Oddworld (USA) (Disc 1).chd"), disc("Oddworld (USA) (Disc 2).chd"),
            disc("Oddworld (Europe) (Disc 1).chd"), disc("Oddworld (Europe) (Disc 2).chd"),
            disc("Oddworld (USA) (Rev 1) (Disc 1).chd"),
            disc("Oddworld (USA) (Disc 2).chd", "/backup")
        )
        val grouped = groupLibraryDiscs(files)
        assertEquals(4, grouped.size)
        assertEquals(2, grouped.count { it.discs.size == 2 })
    }

    @Test fun neverHidesAmbiguousDuplicatesOrSingleDiscFiles() {
        val files = listOf(disc("Oddworld (Disc 1).chd"), disc("Oddworld (Disc 1).iso"),
            disc("Oddworld (Disc 2).chd"), disc("Oddworld 2.chd"), disc("Oddworld (Disc 3).chd", "/other"))
        assertEquals(files, groupLibraryDiscs(files))
    }

    @Test fun acceptsCommonNumberedNamesAndPreservesGameNumbers() {
        val files = listOf(disc("Final Fantasy VII CD2.iso"), disc("Final Fantasy VII CD1.iso"),
            disc("Final Fantasy VIII CD1.iso"), disc("Final Fantasy VIII CD2.iso"))
        val groups = groupLibraryDiscs(files)
        assertEquals(2, groups.size)
        assertEquals(listOf(1, 2), groups.first().discs.map { it.number })
        assertEquals(2, groupLibraryDiscs(listOf(disc("Game - Disk_01.bin"), disc("Game - Disk_02.bin"))).single().discs.size)
    }

    @Test fun resolvesEncodedSafParentsWithoutMixingDifferentDirectories() {
        val first = disc("Game (Disc 1).chd").copy(path = "content://docs/tree/primary%3AGames/document/primary%3AGames%2FGame%2Fone")
        val second = disc("Game (Disc 2).chd").copy(path = "content://docs/tree/primary%3AGames/document/primary%3AGames%2FGame%2Ftwo")
        val foreign = second.copy(path = "content://docs/tree/primary%3AGames/document/primary%3AGames%2FOther%2Ftwo")
        assertEquals(1, groupLibraryDiscs(listOf(second, first)).size)
        assertEquals(2, groupLibraryDiscs(listOf(first, second, foreign)).size)
    }

    @Test fun disablingGroupingReturnsOriginalFiles() {
        val files = listOf(disc("Game (Disc 1).chd"), disc("Game (Disc 2).chd"))
        assertEquals(files, groupLibraryDiscs(files, enabled = false))
    }
}
