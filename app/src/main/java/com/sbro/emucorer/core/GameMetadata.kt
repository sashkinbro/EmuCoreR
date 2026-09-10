package com.sbro.emucorer.core

data class GameMetadata(
    val title: String,
    val serial: String? = null,
    val serialWithCrc: String? = null
)
