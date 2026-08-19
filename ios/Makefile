# Convenience targets for the iOS client. The server itself is built with
# CMake — see CLAUDE.md. Everything here but `icon` runs on the macOS build
# machine, which has its own checkout.

# A formatter in the pipe below must not be able to turn a failed build into a
# successful make: without pipefail the exit status is the last command's, so
# `xcodebuild | anything` always succeeds.
SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c

XCODEGEN := xcodegen
XCODEBUILD := xcodebuild
INKSCAPE ?= inkscape
MAGICK ?= magick
PROJECT := GainDrive.xcodeproj
SCHEME := GainDrive

# The version comes from the repository root and nowhere else; project.yml
# refers to these two by name and XcodeGen substitutes them from the
# environment, so they have to be exported rather than merely set. Named for
# gaindrive rather than for the Xcode settings they end up in, so that nothing
# reads as a build setting referring to itself.
export GAINDRIVE_VERSION := $(shell sed -n 's/^version=//p' ../VERSION)
export GAINDRIVE_BUILD := $(shell sed -n 's/^build=//p' ../VERSION)

LOGO := ../graphics/gaindrive.svg
ICON := Resources/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png
ICON_RAW := build/appicon-raw.png
LOGO_ASSET := Resources/Assets.xcassets/Logo.imageset/gaindrive.svg

# Override on the command line for a different simulator, e.g.
#   make test DEST='platform=iOS Simulator,name=iPad Pro 13-inch (M4)'
DEST ?= platform=iOS Simulator,name=iPhone 16

# The Mac Catalyst destination — Xcode's "My Mac (Mac Catalyst)". The variant
# is what distinguishes it from a native macOS build, which this project does
# not have; omitting it asks for a platform the target does not support.
DEST_MAC ?= platform=macOS,variant=Mac Catalyst

# xcodebuild echoes the entire compiler invocation for every file it touches,
# which buries the handful of lines that matter — a failing build is thousands
# of lines with four that say `error:`, and they are not at the end. `-quiet`
# prints warnings, errors and the result and nothing else; what is left is
# already clang-shaped, source line and caret included.
#
# `make build V=1` restores the full output, for when the problem is the build
# system rather than the code — a missing file, a signing failure, a script
# phase — which is exactly what -quiet hides.
ifeq ($(V),1)
XCFLAGS :=
else
XCFLAGS := -quiet
endif

# Let xcodebuild create or update the provisioning profile itself.
#
# Xcode's GUI does this silently; xcodebuild refuses unless asked, and says so
# with an error that names neither the cause nor the fix — "no profiles for
# org.gaindrive.ios were found". The cause is always the same: the entitlements
# changed, so the existing profile no longer matches the app. `keychain-access-
# groups` was the change that first triggered it, and any future capability will
# do it again.
#
# The cost, stated plainly: with this flag a build may contact Apple's developer
# portal and create or modify a profile on the signed-in account. It does
# nothing when the profile is already valid. Set SIGNING= to switch it off and
# manage profiles by opening the project in Xcode instead.
SIGNING ?= -allowProvisioningUpdates

# xcbeautify, if it happens to be installed, groups diagnostics per file and
# colours them. Optional on purpose: nothing in this build may depend on a tool
# that is not in the repository, so its absence costs only the colours.
#
#   brew install xcbeautify
BEAUTIFY := $(if $(filter 1,$(V)),,$(shell command -v xcbeautify 2>/dev/null))
PIPE := $(if $(BEAUTIFY),| $(BEAUTIFY),)

.PHONY: generate open build build-mac test test-mac art icon logo clean help

# Default target, so `make` alone leaves a project ready to open. Regenerating
# is idempotent and cheap; Xcode notices the project changed under it.
generate: Local.xcconfig
	@test -n "$(GAINDRIVE_VERSION)" -a -n "$(GAINDRIVE_BUILD)" || { \
	  echo "Could not read version and build from ../VERSION."; exit 1; }
	$(XCODEGEN) generate
	@grep -q 'MARKETING_VERSION = $(GAINDRIVE_VERSION);' $(PROJECT)/project.pbxproj || { \
	  echo "xcodegen did not substitute the version into the project — it is"; \
	  echo "probably too old for \$${VAR} expansion in project.yml."; exit 1; }

open: generate
	open $(PROJECT)

build: generate
	$(XCODEBUILD) build -project $(PROJECT) -scheme $(SCHEME) \
	 -destination '$(DEST)' $(SIGNING) $(XCFLAGS) $(PIPE)

build-mac: generate
	$(XCODEBUILD) build -project $(PROJECT) -scheme $(SCHEME) \
	 -destination '$(DEST_MAC)' $(SIGNING) $(XCFLAGS) $(PIPE)

test: generate
	$(XCODEBUILD) test -project $(PROJECT) -scheme $(SCHEME) \
	 -destination '$(DEST)' $(SIGNING) $(XCFLAGS) $(PIPE)

# Worth running on both: the bundle assertions cover Info.plist keys that
# Catalyst treats differently from iOS, and a derived bundle identifier or a
# resource that failed to reach the Mac bundle shows up here rather than as a
# puzzling runtime failure.
test-mac: generate
	$(XCODEBUILD) test -project $(PROJECT) -scheme $(SCHEME) \
	 -destination '$(DEST_MAC)' $(SIGNING) $(XCFLAGS) $(PIPE)

# Both assets derive from graphics/gaindrive.svg rather than being second
# copies of it, so they cannot drift from the artwork or from each other.
#
# Deliberately a prerequisite of nothing. The results are committed, and an
# ordinary build must not need Inkscape and ImageMagick on the machine doing
# it; the macOS build machine has neither. Run this on the Linux tree when the
# artwork changes, and commit what it produces.
art: icon logo

# The app icon. graphics/gaindrive.svg is already a full-bleed square — brand
# red to the edge, sand disc inside — so this is a straight rasterise with no
# padding or background of its own.
icon: $(ICON)

# The in-app logo, kept as a vector so it is drawn at whatever size a view asks
# for. Exported as plain SVG rather than copied: Inkscape's own file carries
# sodipodi and inkscape namespaces, and actool accepts only a subset of SVG, so
# handing it the editor's working file is asking a strict parser to tolerate
# things it has no reason to. The plain export is five elements and renders
# pixel-identically.
logo: $(LOGO_ASSET)

$(LOGO_ASSET): $(LOGO)
	@mkdir -p $(dir $@)
	$(INKSCAPE) $< --export-type=svg --export-plain-svg --export-filename=$@

# Rendered at 2048 and downsampled, because supersampling antialiases the
# grooves better than asking Inkscape for 1024 directly.
#
# `-flatten -alpha off` is not cosmetic: the SVG's red rectangle stops half a
# pixel short of its canvas, and the App Store rejects an icon PNG that carries
# an alpha channel at all. The background colour is the same brand red, so the
# flatten is invisible where the artwork does reach the edge.
$(ICON): $(LOGO)
	@mkdir -p $(dir $(ICON_RAW))
	$(INKSCAPE) $< --export-type=png -w 2048 -h 2048 \
	 --export-filename=$(ICON_RAW)
	$(MAGICK) $(ICON_RAW) -background '#a31623' -flatten -alpha off \
	 -resize 1024x1024 -strip $@
	@rm -f $(ICON_RAW)

# The generated project and its build products only; project.yml is the thing
# that is kept, and so is the rendered icon.
clean:
	rm -rf $(PROJECT) DerivedData build

# Written rather than committed because the team id is per developer, and a
# regenerated project must not carry someone else's. project.yml references it
# unconditionally, so it has to exist before xcodegen runs.
Local.xcconfig:
	@printf '%s\n' \
	 '// Machine-local build settings. Not in version control.' \
	 '//' \
	 '// Set DEVELOPMENT_TEAM to your ten-character Apple Developer Team ID to' \
	 '// build on a device. The simulator does not need one.' \
	 'DEVELOPMENT_TEAM =' > $@
	@echo "Wrote $@ — set DEVELOPMENT_TEAM in it to build on a device."

help:
	@echo "Targets:"
	@echo "  generate   Regenerate GainDrive.xcodeproj from project.yml (default)"
	@echo "  open       Regenerate and open the project in Xcode"
	@echo "  build      Build for \$$DEST (an iOS simulator by default)"
	@echo "  build-mac  Build for \$$DEST_MAC (My Mac, Mac Catalyst)"
	@echo "  test       Run the unit tests on \$$DEST"
	@echo "  test-mac   Run the unit tests on \$$DEST_MAC"
	@echo "  art        Re-derive both assets from graphics/gaindrive.svg"
	@echo "             (needs Inkscape and ImageMagick; not part of a build)"
	@echo "  icon       Just the app icon PNG"
	@echo "  logo       Just the in-app logo SVG"
	@echo "  clean      Remove the generated project and DerivedData"
	@echo
	@echo "Build output is quiet by default: warnings, errors and the result"
	@echo "only. Pass V=1 for xcodebuild's full output."
