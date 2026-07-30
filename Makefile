# Convenience targets for the Android client. The server itself is built with
# CMake — see CLAUDE.md.

ANDROID  := android
GRADLEW  := ./gradlew
APK      := $(ANDROID)/app/build/outputs/apk/release/app-release.apk

.PHONY: help install-device

help:
	@echo "Targets:"
	@echo "  install-device   Build the release APK and install it on the"
	@echo "                   USB-connected device."

# Two steps rather than `gradlew installRelease`: that task installs to every
# attached target, so an emulator running alongside the phone would get it too
# (or the build would fail for the ambiguity). `adb -d` means specifically the
# one device on USB.
#
# -r reinstalls over the existing app, keeping its data. That works because both
# variants share an application id and are signed with the same debug key.
install-device:
	cd $(ANDROID) && $(GRADLEW) assembleRelease
	adb -d install -r $(APK)
