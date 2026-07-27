package org.gaindrive.android.data.model

/**
 * Light/dark choice, matching the three-way setting the web client stores in
 * `localStorage`. Global rather than per server — it describes the phone.
 */
enum class ThemeMode { AUTO, LIGHT, DARK }
