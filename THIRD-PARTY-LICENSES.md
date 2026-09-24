# Third-party licences

GainDrive itself is GPLv3-or-later (see `LICENSE`). It incorporates the
components listed below, which keep their own licences - this file exists to
retain their notices, as several of those licences require.

Everything distributed here is one-way compatible into GPLv3: the combined
work is GPLv3, but the individual components are not relicensed and may still
be taken from upstream under their own terms.

Keep this file in step with `CMakeLists.txt`,
`android/gradle/libs.versions.toml` and `android/app/build.gradle.kts`.


## Server - vendored in this repository

Copied verbatim into `third_party/`, with their licence texts alongside. None of
them can be relied on from a package manager; `third_party/README.md` explains
why. A version change here is a manual `cp` and must be recorded in this file in
the same commit.

* cpp-httplib 0.54.1 - `third_party/httplib.h`, `third_party/httplib.LICENSE`
  MIT · Copyright (c) 2025 Yuji Hirose
  https://github.com/yhirose/cpp-httplib

* mdns 1.4.3 - `third_party/mdns.h`, `third_party/mdns.LICENSE`
  Public domain (Unlicense) · Mattias Jansson
  https://github.com/mjansson/mdns

* stb - `third_party/stb_image.h` (v2.30),
  `third_party/stb_image_resize2.h` (v2.18),
  `third_party/stb_image_write.h` (v1.16), `third_party/stb.LICENSE`
  Dual-licensed: public domain (Unlicense) *or* MIT, at the recipient's
  choice · Copyright (c) 2017 Sean Barrett
  https://github.com/nothings/stb
  Both arms are GPLv3-compatible, so nothing in the compatibility notes below
  turns on which is taken. Upstream publishes no releases and no version
  numbers outside the headers, which is why these carry the versions from
  their own first lines rather than a tag.

The web client's icon font is vendored the same way, but lives in `web/`
rather than `third_party/` because it is embedded into the binary by
`cmake/embed_web.cmake` along with the rest of the client:

* Material Symbols Rounded (Google Fonts release v368) -
  `web/material-symbols-rounded.woff2`,
  `web/material-symbols-rounded.LICENSE`
  Apache-2.0 · Copyright Google LLC
  https://github.com/google/material-design-icons
  Taken verbatim from the file Google Fonts serves for
  `Material+Symbols+Rounded:opsz,wght,FILL,GRAD@24,400,0..1,0`: the whole
  icon set with `opsz`, `wght` and `GRAD` pinned and only `FILL` left as a
  variable axis. It is shipped whole rather than subset because the icons are
  addressed by ligature, so a subset keyed on the names actually used still
  drags in every icon spellable from the same letters - measured at 374 KB
  against 537 KB, for the price of a build-time dependency on Python
  fontTools. Not worth it.

  **Two glyphs of it are also in the iOS app**, as
  `ios/Resources/Assets.xcassets/CastIcon.imageset/cast.svg` and
  `CastConnectedIcon.imageset/cast_connected.svg` - extracted from this same
  file rather than vendored again, so there is one copy of the artwork and one
  licence entry. SF Symbols has no cast icon and will not get one, Cast being
  Google's mark. `ios/Resources/Assets.xcassets/README.md` says how to
  regenerate them; fontTools is needed then and not at build time.


## Server - system packages

**Whether these are linked from the system or built from source is a property
of the build host.** The default build takes whatever the distribution provides
at or above the floor listed and compiles the rest from the pinned versions
below; `GAINDRIVE_BUNDLED_DEPS` forces a source build regardless, and
`GAINDRIVE_ALLOW_FETCH=OFF` turns a missing package into an error rather than a
source build, so that nothing can end up bundled unintentionally.

The practical consequence for anyone redistributing a gaindrive binary: the
versions recorded here apply to the components their build actually compiled in.
For the rest, the notices that travel with the binary are the distribution's.
Configure prints which is which (`gaindrive: system packages: ...` /
`gaindrive: built from source: ...`); the released static binaries compile all
of them, so for those every version below applies.

* cxxopts 3.3.1 (floor 3.0)
  MIT · Copyright (c) 2014 Jarryd Beck
  https://github.com/jarro2783/cxxopts

* nlohmann/json 3.12.0 (floor 3.11)
  MIT · Copyright (c) 2013-2025 Niels Lohmann
  https://github.com/nlohmann/json

* SQLiteCpp 3.3.2 (floor 3.3)
  MIT · Copyright (c) 2012-2024 Sébastien Rombauts
  https://github.com/SRombauts/SQLiteCpp

* SQLite3
  Public domain · the system library in the default build; bundled inside
  SQLiteCpp and not fetched separately under `GAINDRIVE_BUNDLED_DEPS`
  https://www.sqlite.org/copyright.html

* TagLib 2.0.2 (floor 2.0)
  LGPL-2.1-or-later OR MPL-1.1 · Copyright (c) Scott Wheeler and contributors
  Used under the LGPL arm; see the note below.
  https://github.com/taglib/taglib

* tinyxml2 10.0.0 (floor 9.0)
  zlib · Copyright (c) Lee Thomason
  https://github.com/leethomason/tinyxml2

* reproc / reproc++ 14.2.5 (floor 14.2)
  MIT · Copyright (c) Daan De Meyer
  https://github.com/DaanDeMeyer/reproc

* libarchive 3.8.9 (floor 3.6)
  BSD-2-Clause · Copyright (c) Tim Kientzle and contributors
  https://github.com/libarchive/libarchive

* OpenSSL
  Apache-2.0 (3.x) · Copyright (c) The OpenSSL Project Authors
  `find_package(OpenSSL REQUIRED)` in every mode. Must be 3.x; see the note
  below.
  https://www.openssl.org/


## Server - reached only by static builds

Pulled in through libarchive and httplib when `GAINDRIVE_STATIC` is set
(`CMakeLists.txt`). All GPL-compatible; versions are whatever the build host
provides.

The released binaries add `GAINDRIVE_STATIC_LIBC` on top and are built in the
Alpine container defined by `cmake/static_build.cmake`, so for those the build
host is pinned and musl libc is linked in as well. Alpine also settles the
OpenSSL question below: it ships OpenSSL 3.x, which a redistributable static
build requires.

* musl libc
  MIT · Copyright (c) Rich Felker and contributors
  Statically linked into the released binaries only; a glibc build links glibc
  dynamically and never includes it.
  https://musl.libc.org/

* zlib
  zlib licence · Copyright (c) Jean-loup Gailly and Mark Adler
  https://zlib.net/

* bzip2
  BSD-style · Copyright (c) Julian Seward
  https://sourceware.org/bzip2/

* xz / liblzma
  0BSD (public domain in older releases) · Copyright (c) Lasse Collin and
  contributors
  https://tukaani.org/xz/

* zstd
  BSD-3-Clause OR GPL-2.0 · Copyright (c) Meta Platforms, Inc.
  Used under the BSD arm.
  https://github.com/facebook/zstd


## Android app - shipped in the APK

From `android/gradle/libs.versions.toml` and `android/app/build.gradle.kts`.
**Every one of these is Apache-2.0.**

* AndroidX / Jetpack · Copyright (c) The Android Open Source Project
  core-ktx 1.15.0, activity-compose 1.9.3, lifecycle 2.8.7,
  navigation-compose 2.8.5, Compose BOM 2024.12.01 (ui, ui-graphics,
  material3, material-icons-extended, ui-tooling-preview,
  material3-adaptive-navigation-suite, material3.adaptive:adaptive and
  :adaptive-layout), datastore-preferences 1.1.1, room 2.6.1,
  mediarouter 1.7.0, hilt-navigation-compose 1.2.0
  https://developer.android.com/jetpack/androidx

* AndroidX Window · Copyright (c) The Android Open Source Project
  Transitive, via material3.adaptive:adaptive, which reads the window size
  and the folding posture through it.
  https://developer.android.com/jetpack/androidx/releases/window

* AndroidX Media3 1.5.1 · Copyright (c) The Android Open Source Project
  exoplayer, exoplayer-hls, session, ui, datasource, datasource-okhttp,
  database
  https://github.com/androidx/media

* Kotlin standard library, kotlinx.serialization 1.7.3,
  kotlinx.coroutines 1.9.0
  Copyright (c) JetBrains s.r.o. and Kotlin Programming Language contributors
  https://github.com/JetBrains/kotlin

* Dagger and Hilt 2.52 · Copyright (c) Google LLC
  https://github.com/google/dagger

* Guava · Copyright (c) The Guava Authors
  Transitive, via media3-common and kotlinx-coroutines-guava.
  https://github.com/google/guava

* Retrofit 2.11.0, OkHttp 4.12.0 (with okhttp-sse and logging-interceptor),
  Okio · Copyright (c) Square, Inc.
  https://square.github.io/okhttp/

* retrofit2-kotlinx-serialization-converter 1.0.0
  Copyright (c) Jake Wharton
  https://github.com/JakeWharton/retrofit2-kotlinx-serialization-converter

* Coil 3.0.4 · Copyright (c) Coil Contributors
  coil-compose, coil-network-okhttp
  https://github.com/coil-kt/coil

The Cast protocol is a port of `src/castmanager.cc`, not a dependency -
neither `media3-cast` nor `play-services-cast-framework` is used. That is
deliberate: Google Play Services is proprietary and would be incompatible with
GPLv3. Only the Apache-2.0 `androidx.mediarouter` route-picker UI is needed.


## iOS app - shipped in the bundle

The iOS app currently has **no third-party dependencies**; `ios/project.yml`
declares no Swift packages, and the phase 0 skeleton links only Apple
frameworks. Two are planned, recorded here so
the licence question is settled before either is adopted:

* GRDB · MIT · Copyright (c) Gwendal Roué
  https://github.com/groue/GRDB.swift

* Nuke · MIT · Copyright (c) Alexander Grebenyuk
  or Kingfisher · MIT · Copyright (c) Wei Wang - undecided
  https://github.com/kean/Nuke · https://github.com/onevcat/Kingfisher

Both are MIT and therefore fine. Add them to this section when they actually
land in `project.yml`, not before.

Note that `ios/` is GPLv3 **with the Application Distribution Exception**
(`ios/LICENSE`), which permits app-store distribution of the binary but
explicitly does not permit linking against non-free libraries. Google's Cast
SDK for iOS is proprietary and so remains excluded, notwithstanding that it is
an ordinary Swift Package Manager dependency.


## Build- and test-only - not distributed

These never reach a release artifact, so their licences do not affect
redistribution of GainDrive.

* Android Gradle Plugin 8.10.1, Gradle, KSP 2.0.21-1.0.28, the Room and Hilt
  annotation processors · Apache-2.0

* Gradle Play Publisher 3.13.0 · MIT · Copyright (c) Christian Becker, Björn
  Hurling, Alexandre Saveau
  https://github.com/Triple-T/gradle-play-publisher
  Uploads the release App Bundle to Google Play (`make publish`). It runs on
  the Gradle classpath only and nothing of it is in the APK. It pulls in
  Google's API client libraries (Apache-2.0) for the same classpath. Held at
  3.x because 4.0.0 requires AGP 9.

* Compose ui-tooling and ui-test-manifest (`debugImplementation`),
  ui-test-junit4, androidx.test ext-junit 1.2.1, Espresso 3.6.1 · Apache-2.0
  Copyright (c) The Android Open Source Project

* MockWebServer 4.12.0 · Apache-2.0 · Copyright (c) Square, Inc.

* kotlinx-coroutines-test 1.9.0 · Apache-2.0 · Copyright (c) JetBrains s.r.o.

* **JUnit 4.13.2 · EPL-1.0**, which the FSF classes as *not* GPL-compatible.
  This is harmless here because it is declared `testImplementation` only: it
  is never on the release classpath, never in the APK, and test code is not
  distributed. Do not promote it to `implementation`.
  https://junit.org/junit4/

* Hamcrest · BSD-3-Clause · transitive, via JUnit 4.

* XcodeGen · MIT · Copyright (c) Yonas Kolb
  https://github.com/yonaskolb/XcodeGen
  Generates `ios/GainDrive.xcodeproj` from `ios/project.yml`. Installed with
  Homebrew on the build machine and not vendored, so nothing of it is in the
  repository or the app.

* xcbeautify · MIT · Copyright (c) Thomas Hempel and contributors
  https://github.com/cpisciotta/xcbeautify
  **Optional.** `ios/Makefile` pipes `xcodebuild` through it when it happens to
  be installed and runs unchanged when it is not, so this is a convenience on
  one developer's machine rather than a dependency. Listed anyway because the
  Makefile names it.

* Swift Testing · Apache-2.0 with LLVM exception · Copyright (c) Apple Inc.
  https://github.com/swiftlang/swift-testing
  Used by `ios/GainDriveTests/`. It ships with the Xcode toolchain rather than
  being resolved as a package, and the framework is linked into the test
  bundle only - never into the app. XCTest, likewise part of the toolchain, is
  in the same position. Note the contrast with JUnit above: this is the
  test-framework slot, and on iOS it happens to be GPL-compatible anyway.


## Invoked as separate programs, not linked

* ffmpeg and ffprobe
  gaindrive runs these as child processes through reproc++ for transcoding,
  remuxing and metadata probing. Running a program is not linking against it,
  so ffmpeg's licence (LGPL or GPL depending on how it was built) does not
  propagate to GainDrive, and no ffmpeg code is distributed here.

* yt-dlp · Unlicense (public domain)
  https://github.com/yt-dlp/yt-dlp
  gaindrive runs this as a child process through reproc++ when a configured
  `url_handlers` entry matches a pasted URL. Same reasoning as ffmpeg: running
  a program is not linking against it, and no yt-dlp code is distributed here.
  It is optional and named only by configuration - with no yt-dlp on PATH the
  URL-fetch endpoints report no handlers and the client does not offer the
  option. Note it invokes ffmpeg in turn, which gaindrive already requires.
  Nothing in the code depends on it specifically: the handler table names
  whatever tool the operator chooses, and the built-in default is the only
  place yt-dlp is mentioned at all.


## Compatibility notes

These are the points worth not rediscovering.

* **Apache-2.0 is incompatible with GPLv2.** Its patent-termination clause is
  an added restriction GPLv2 does not permit; GPLv3 §7 accommodates it. Since
  the entire Android dependency set is Apache-2.0, "GPLv3 or later" is
  available to this project and "GPLv2 or later" is not.

* **TagLib is taken under the LGPL-2.1 arm** of its dual licence. The MPL-1.1
  arm is GPL-incompatible, so the choice matters. LGPL-2.1 §3 explicitly
  permits applying the ordinary GPL to a copy, which also settles the static
  linking question: LGPL-2.1 §6 would otherwise require shipping object files
  so a user could relink, and GPLv3's source requirement is strictly stronger
  than that.

* **OpenSSL must be 3.x.** Version 3.0 relicensed to Apache-2.0. OpenSSL
  1.1.1 and earlier are under the OpenSSL and SSLeay licences, whose
  advertising clause makes them GPL-incompatible without a linking exception.
  This is a real constraint on the build host, not a formality - a static
  build against 1.1.1 would not be redistributable under GPLv3.

* MIT, BSD-2/3-Clause, zlib, 0BSD and public-domain components impose nothing
  beyond retaining their notices, which is what this file does.
