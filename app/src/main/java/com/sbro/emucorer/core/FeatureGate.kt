package com.sbro.emucorer.core

import android.content.Context
import com.sbro.emucorer.BuildConfig

object FeatureGate {
    const val OFFICIAL_PACKAGE = "com.sbro.emucorer"
    private const val BETA_PACKAGE = "com.sbro.emucorex.beta"
    private const val EXPECTED_KEY_DIGEST =
        "f91428fee626a9366dbfdbc6e3edc0129db2e29a36ef28207260cf369a298413"

    fun isAllowedPackage(packageName: String): Boolean =
        packageName == OFFICIAL_PACKAGE ||
            packageName == BETA_PACKAGE ||
            packageName.startsWith("$OFFICIAL_PACKAGE.")

    fun hasValidKey(): Boolean =
        BuildConfig.FEATURE_KEY_DIGEST.isNotEmpty() &&
            BuildConfig.FEATURE_KEY_DIGEST == EXPECTED_KEY_DIGEST

    fun isEnabled(context: Context): Boolean =
        isAllowedPackage(context.packageName) && hasValidKey()
}
