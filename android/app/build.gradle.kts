plugins {
	alias(libs.plugins.android.application)
	alias(libs.plugins.kotlin.android)
	alias(libs.plugins.kotlin.compose)
	alias(libs.plugins.kotlin.serialization)
	alias(libs.plugins.ksp)
	alias(libs.plugins.hilt)
}

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

	buildTypes {
		debug {
			applicationIdSuffix = ".debug"
			versionNameSuffix = "-debug"
		}
		release {
			isMinifyEnabled = true
			isShrinkResources = true
			proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
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
	implementation(libs.media3.session)
	implementation(libs.media3.ui)
	implementation(libs.media3.datasource.okhttp)

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

	testImplementation(libs.junit)
	testImplementation(libs.kotlinx.coroutines.test)
	testImplementation(libs.okhttp.mockwebserver)

	androidTestImplementation(libs.androidx.junit)
	androidTestImplementation(libs.androidx.espresso.core)
	androidTestImplementation(platform(libs.androidx.compose.bom))
	androidTestImplementation(libs.androidx.compose.ui.test.junit4)
	debugImplementation(libs.androidx.compose.ui.test.manifest)
}
