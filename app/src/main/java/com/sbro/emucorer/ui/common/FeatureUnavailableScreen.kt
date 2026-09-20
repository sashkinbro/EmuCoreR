package com.sbro.emucorer.ui.common

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.sbro.emucorer.R
import com.sbro.emucorer.ui.theme.ScreenHorizontalPadding
import com.sbro.emucorer.ui.theme.neon.LocalNeonTheme
import com.sbro.emucorer.ui.theme.neon.neonShape

@Composable
fun FeatureUnavailableScreen(
    title: String,
    onBackClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
    ) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                start = ScreenHorizontalPadding,
                end = ScreenHorizontalPadding,
                bottom = 24.dp
            ),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            item {
                ScreenTopBar(
                    title = title,
                    onBackClick = onBackClick,
                    modifier = Modifier.padding(top = appScreenTopPadding())
                )
            }
            item {
                Surface(
                    modifier = Modifier.fillMaxWidth(),
                    shape = neonShape(22.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(
                        alpha = if (LocalNeonTheme.current) 0.22f else 0.34f
                    )
                ) {
                    Text(
                        text = stringResource(R.string.feature_gate_unavailable_body),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 20.dp, vertical = 18.dp)
                    )
                }
            }
        }
    }
}
