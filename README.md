# GainDrive

GainDrive is a Subsonic-compatible music server along with an embedded
web player and native Android and iOS clients. The server and clients
support most of the Subsonic protocol, along with some OpenSubsonic
extensions. Key server features include:

  * Single binary (about 10MB), written in modern C++,
  * Music catalogue stored in SQLite3 database,
  * Separate SQLite3 database for user data (accounts, playlists),
  * Supports audio and video,
  * Automatic library scanning/updating,
  * Multi-disc album support,
  * Fetches artist/album metadata from MusicBrainz, Discogs and TMDB,
  * Fetches film and series posters and descriptions from TMDB,
  * Support for Chromecast from the server,
  * Multiple user accounts.

GainDrive was written largely as an experiment in AI-supported
development. The code was written by Claude, but the design and
architecture are mine.


# Download

Prebuilt binaries for x86_64 and arm64 Linux are attached to every
[release](https://github.com/kpeeters/gaindrive/releases). They are
statically linked, so there is nothing to install alongside them.
So simply do e.g.:

```
      curl -LO https://github.com/kpeeters/gaindrive/releases/latest/download/gaindrive-linux-x86_64
      chmod +x gaindrive-linux-x86_64
```

The one thing gaindrive expects to find on the system is `ffmpeg` (and
`ffprobe`), on `PATH`. Without them the library still browses and plays,
but transcoding, video and scaled cover art do not work.



# Building from source

Building needs only cmake, a C++20 compiler and OpenSSL:

```
      cd gaindrive
	  cmake -B build
	  cmake --build build
```

GainDrive prefers system libraries and builds from source only what it
cannot find. Configure prints which it used for each, so there is no
guessing:

```
      -- gaindrive: system packages: tinyxml2, cxxopts, nlohmann_json, ...
      -- gaindrive: built from source: TagLib, reproc++
```

Installing the development packages first is worth it; the build is
much quicker and the libraries then get security updates from your
distribution rather than being frozen into the binary.

Debian 13 or newer, and derivatives:

```
      sudo apt install build-essential cmake pkg-config \
           libtag-dev libtinyxml2-dev libcxxopts-dev nlohmann-json3-dev \
           libsqlitecpp-dev libreproc-dev libarchive-dev libssl-dev
```

macOS:

```
      brew install cmake pkgconf ffmpeg openssl@3 \
           taglib tinyxml2 cxxopts nlohmann-json sqlitecpp reproc libarchive
```

If the version of one of these packages on your system is too low, 
the build process will fetch the source automatically and compile it
into the binary (e.g. on Ubuntu 24.04 the version of TagLib is 1.13 
while gaindrive needs 2.0).

```
      sudo apt install build-essential cmake pkg-config \
           libtinyxml2-dev libcxxopts-dev nlohmann-json3-dev \
           libsqlitecpp-dev libarchive-dev libssl-dev
```

Two options control this:

  * `GAINDRIVE_BUNDLED_DEPS` - build these from source even when the
    system has them. A list, or `ALL`. The names are `TagLib`,
    `tinyxml2`, `cxxopts`, `nlohmann_json`, `SQLiteCpp`, `reproc++` and
    `LibArchive`.
  * `GAINDRIVE_ALLOW_FETCH` - on by default. Set it to `OFF` and a
    missing package becomes an error instead of a download. That is what
    a distribution packager wants, since a package must neither bundle
    copies nor fetch during a build, and it is also how to guarantee an
    offline build.

Anything built from source is fetched from git, so `ALLOW_FETCH=OFF`, or
simply having every package installed, is what makes the build need no
network at all.

The above produces an ordinary dynamically linked binary. To reproduce
the released, fully static one instead:

```
      cmake --build build --target static
```

This runs the build inside an Alpine container (docker or podman) and
leaves the result in `build-static/`. Everything it does lives in
`cmake/static_build.cmake`, which also runs standalone as `cmake -P
cmake/static_build.cmake` if you have no configured tree. This uses musl 
instead of glibc to avoid issues with `getaddrinfo`.



# Folder conventions

GainDrive intentionally presents all information only in *one* way, 
whether your client retrieves it using the ID3 tag retrieval calls
or the folder-browsing endpoints. It will look at ID3 tags, but it
will get artist and album names by assuming that everything is
stored in one of the following ways: 

All of these are relative to a root, so the full stored path of the first
example is `music/Artist/Album/01-track.mp3`.

* Single-disc audio (in an **artist** root):
  ```
      Artist/
	     Album/
		    01-track.mp3
			02-track.mp3
  ```

* Multi-disc audio:
  ```
      Artist/
	     Album/
		    Disc1/
		       01-track.mp3
			   02-track.mp3
		    Disc2/
		       01-track.mp3
			   02-track.mp3
  ```

* Music (concert) video (in an **artist** root):
  ```
      Artist/
	     Concert/
  		    01-track.mp4
			02-track.mp4
   ```

* Other video, e.g. movies (in a **category** root):
  ```
      Category/
	     Name/
  		    01-track.mp4
   ```

* Series video / multi-disc video (in a **category** root):
  ```
      Category/
	     Name/
		    Series01/
  		       01-track.mp4
			   02-track.mp4
		    Series02/
  		       01-track.mp4
			   02-track.mp4
   ```

* DVD rip (in a **category** or **artist** root):
  ```
      Category/
	     Name/
		    VIDEO_TS/
			   VTS_01_1.VOB
			   VTS_01_2.VOB
			   VTS_02_1.VOB
   ```
  A folder containing `VIDEO_TS` is recognised as a DVD rip. Each titleset
  becomes one track, named `Title 1`, `Title 2` and so on - DVD titles carry
  numbers, not names. Menu VOBs (`VTS_nn_0.VOB`) and titlesets too small to be
  anything but a logo are skipped. The parts of a titleset are joined back
  together on playback, since a DVD splits one continuous stream across 1 GB
  files.

* A single item, with no folder of its own - one documentary, one home video,
  one clip. There is often nothing to put in a subdirectory:
  ```
      Category/
	     makingcheese.mp4
	     makingcheese.jpg
	     Name/
  		    01-track.mp4
   ```

So essentially, it's a three-layer structure with a possible "disc" 
layer added on top. You do not need to make this fourth layer 
explicit in the folder structure; using ID3 "disc" or "series" 
tags works too.


# Install as a systemd service

On Linux, gaindrive installs itself. Get it working in the foreground first,
then re-run the same command with `sudo` and `--install-service`:

      # 1. Create the first account. It will be an administrator.
      gaindrive --db ~/gaindrive/gaindrive.db

      # 2. Check that it serves what you expect.
      gaindrive --db ~/gaindrive/gaindrive.db \
                --artist-root   music=/srv/music \
                --category-root movies=/srv/movies

      # 3. Same options, plus sudo and one flag.
      sudo gaindrive --db ~/gaindrive/gaindrive.db \
                     --artist-root   music=/srv/music \
                     --category-root movies=/srv/movies \
                     --install-service

      sudo systemctl start gaindrive

The third step writes `/etc/gaindrive.conf` from the options you just proved,
writes `/etc/systemd/system/gaindrive.service`, and enables it for the next
boot. It does not start the service, because the server from step 2 may still
be holding the port.


# Licence

GainDrive is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. The full text is in `LICENSE`.

This covers the server (`src/`), the embedded web client (`web/`), the build
files (`cmake/`), the test scripts (`tests/`), the Android app (`android/`)
and the iOS app (`ios/`).

The iOS app carries one **additional permission** under GPLv3 section 7, in
`ios/LICENSE`: it may be distributed through an app store on that store's
usual terms, which would otherwise conflict with the GPL over device limits,
redistribution and code signing. The permission covers distribution of the
binary only. The source stays GPLv3, it does not extend to linking against
proprietary libraries, and it applies to `ios/` alone - no other part of
GainDrive is affected.

GainDrive is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See the GNU General Public License for more details.

Third-party components have their own licences, all of them compatible with
GPLv3. They are listed in `THIRD-PARTY-LICENSES.md`.

