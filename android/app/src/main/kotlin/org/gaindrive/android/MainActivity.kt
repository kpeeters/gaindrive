package org.gaindrive.android

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import dagger.hilt.android.AndroidEntryPoint

/**
 * Placeholder shell. Phase 1 replaces the body with the navigation host and
 * the login/session gate described in PLAN.md.
 */
@AndroidEntryPoint
class MainActivity : ComponentActivity() {
	override fun onCreate(savedInstanceState: Bundle?) {
		super.onCreate(savedInstanceState)
		enableEdgeToEdge()
		setContent {
			MaterialTheme {
				Placeholder()
			}
		}
	}
}

@Composable
private fun Placeholder() {
	Scaffold { insets ->
		Box(
			modifier = Modifier.fillMaxSize().padding(insets),
			contentAlignment = Alignment.Center,
		) {
			Text("GainDrive", style = MaterialTheme.typography.headlineMedium)
		}
	}
}
