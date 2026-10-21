package com.sbro.emucorer.core

enum class GpuVendor {
    ADRENO,
    MALI,
    POWERVR,
    XCLIPSE,
    UNKNOWN
}

enum class AdrenoFamily(val catalogToken: String) {
    A6XX("6xx"),
    A7XX("7xx"),
    A8XX("8xx")
}

data class GpuModelInfo(
    val vendor: GpuVendor,
    val displayName: String,
    val adrenoFamily: AdrenoFamily? = null
)
