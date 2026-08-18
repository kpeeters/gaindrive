import java.util.Properties

plugins {
	alias(libs.plugins.android.application)
	alias(libs.plugins.kotlin.android)
	alias(libs.plugins.kotlin.compose)
	alias(libs.plugins.kotlin.serialization)
	alias(libs.plugins.ksp)
	alias(libs.plugins.hilt)
	alias(libs.plugins.play.publisher)
}

// Release signing. The keystore itself is never in the repository — only a
// pointer to it, from android/keystore.properties or from the environment, so a
// build machine can supply the same thing without a file. Absent both, the
// release variant falls back to the debug key (see buildTypes below), which is
// what keeps `make install-device` working on a machine that will never
// publish. `make keystore` generates one.
val keystoreProps = Properties().apply {
	val f = rootProject.file("keystore.properties")
	if (f.exists()) f.inputStream().use { load(it) }
}

fun keystoreValue(key: String, env: String): String? =
	keystoreProps.getProperty(key) ?: System.getenv(env)

val releaseStore = keystoreValue("storeFile", "GAINDRIVE_KEYSTORE")

android {
	namespace = "org.gaindrive.android"
	compileSdk = 35

	defaultConfig {
		applicationId = "org.gaindrive.android"
		minSdk = 26
		targetSdk = 35
		versionCode = 1
		versionName = "0.1"

		testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
	}

	signingConfigs {
		// ?.let rather than a null check, because a script-level val is a
		// property and so is not smart-cast.
		releaseStore?.let { path ->
			create("release") {
				storeFile = rootProject.file(path)
				storePassword = keystoreValue("storePassword", "GAINDRIVE_KEYSTORE_PASSWORD")
				keyAlias = keystoreValue("keyAlias", "GAINDRIVE_KEY_ALIAS") ?: "upload"
				// One password for both is what keytool produces by default.
				keyPassword = keystoreValue("keyPassword", "GAINDRIVE_KEY_PASSWORD")
					?: storePassword
			}
		}
	}

	buildTypes {
		debug {
			// No applicationIdSuffix: both variants install under the same id, so
			// switching between them replaces the app in place and keeps the
			// configured servers rather than presenting a second, empty install.
			versionNameSuffix = "-debug"
		}
		release {
			isMinifyEnabled = true
			isShrinkResources = true
			proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")

			// The upload key when one is configured, and otherwise the debug
			// key, so `installRelease` still works without any keystore
			// ceremony. That fallback exists to make performance measurable:
			// judging Compose animation smoothness from a debug build is
			// misleading, because debug builds skip R8 and run Compose's
			// unoptimised paths. Play rejects a debug-signed upload, which is
			// why `make publish` refuses before building rather than letting
			// the rejection arrive at the end of a long R8 run.
			signingConfig = signingConfigs.findByName("release")
				?: signingConfigs.getByName("debug")
		}
	}

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}

	buildFeatures {
		compose = true
		// For BuildConfig.DEBUG, which gates the HTTP logging interceptor.
		buildConfig = true
	}

	packaging {
		resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
	}
}

// Google Play upload; driven from the Makefile, which is where the workflow is
// documented. Every property here is also a CLI option on the publish tasks
// (camelCase becomes kebab-case), so `--track` overrides the default per run
// without editing this file.
play {
	// Internal testing: no review wait and nothing user-visible, so a mistake
	// costs nothing. Promotion out of it is a Play Console action.
	track.set("internal")
	// Only affects the plain `publish` task; `publishReleaseBundle` is explicit.
	defaultToAppBundles.set(true)

	// Credentials are the service-account JSON key: this file when it exists,
	// or the whole key in ANDROID_PUBLISHER_CREDENTIALS, which GPP reads by
	// itself when nothing is set here. Neither is in the repository.
	val creds = rootProject.file("play-credentials.json")
	if (creds.exists()) serviceAccountCredentials.set(creds)

	// resolutionStrategy is deliberately left at its default of failing. AUTO
	// would take the next free version code from Play, which makes what was
	// uploaded untraceable to the versionCode in this file; a refused upload
	// naming the clash is more useful, and the fix is to bump versionCode.
}

kotlin {
	compilerOptions {
		jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
	}
}

dependencies {
	implementation(libs.androidx.core.ktx)
	implementation(libs.androidx.activity.compose)
	implementation(libs.androidx.lifecycle.runtime.ktx)
	implementation(libs.androidx.lifecycle.runtime.compose)
	implementation(libs.androidx.lifecycle.viewmodel.compose)
	implementation(libs.androidx.navigation.compose)

	implementation(platform(libs.androidx.compose.bom))
	implementation(libs.androidx.compose.ui)
	implementation(libs.androidx.compose.ui.graphics)
	implementation(libs.androidx.compose.ui.tooling.preview)
	implementation(libs.androidx.compose.material3)
	implementation(libs.androidx.compose.material.icons.extended)
	debugImplementation(libs.androidx.compose.ui.tooling)

	implementation(libs.hilt.android)
	implementation(libs.hilt.navigation.compose)
	ksp(libs.hilt.compiler)

	implementation(libs.media3.exoplayer)
	implementation(libs.media3.exoplayer.hls)
	implementation(libs.media3.session)
	implementation(libs.media3.ui)
	implementation(libs.media3.datasource.okhttp)
	implementation(libs.media3.datasource)
	implementation(libs.media3.database)

	implementation(libs.retrofit)
	implementation(libs.retrofit.serialization)
	implementation(libs.okhttp)
	implementation(libs.okhttp.sse)
	implementation(libs.okhttp.logging)

	implementation(libs.kotlinx.serialization.json)
	implementation(libs.kotlinx.coroutines.android)
	implementation(libs.kotlinx.coroutines.guava)

	implementation(libs.coil.compose)
	implementation(libs.coil.network.okhttp)

	implementation(libs.androidx.datastore.preferences)

	implementation(libs.room.runtime)
	implementation(libs.room.ktx)
	ksp(libs.room.compiler)

	testImplementation(libs.junit)
	testImplementation(libs.kotlinx.coroutines.test)
	testImplementation(libs.okhttp.mockwebserver)

	androidTestImplementation(libs.androidx.junit)
	androidTestImplementation(libs.androidx.espresso.core)
	androidTestImplementation(platform(libs.androidx.compose.bom))
	androidTestImplementation(libs.androidx.compose.ui.test.junit4)
	debugImplementation(libs.androidx.compose.ui.test.manifest)
}
