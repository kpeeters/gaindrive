# Convenience targets for the iOS client. The server itself is built with
# CMake — see CLAUDE.md. Everything here but `icon` runs on the macOS build
# machine, which has its own checkout.

XCODEGEN := xcodegen
XCODEBUILD := xcodebuild
INKSCAPE ?= inkscape
MAGICK ?= magick
PROJECT := GainDrive.xcodeproj
SCHEME := GainDrive

LOGO := ../graphics/gaindrive.svg
ICON := Resources/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png
ICON_RAW := build/appicon-raw.png

# Override on the command line for a different simulator, e.g.
#   make test DEST='platform=iOS Simulator,name=iPad Pro 13-inch (M4)'
DEST ?= platform=iOS Simulator,name=iPhone 16

.PHONY: generate open build test icon clean help

# Default target, so `make` alone leaves a project ready to open. Regenerating
# is idempotent and cheap; Xcode notices the project changed under it.
generate: Local.xcconfig
	$(XCODEGEN) generate

open: generate
	open $(PROJECT)

build: generate
	$(XCODEBUILD) build -project $(PROJECT) -scheme $(SCHEME) -destination '$(DEST)'

test: generate
	$(XCODEBUILD) test -project $(PROJECT) -scheme $(SCHEME) -destination '$(DEST)'

# The app icon is rendered from the shared artwork rather than kept as a second
# copy of it, so the two cannot drift. graphics/gaindrive.svg is already a
# full-bleed square — brand red to the edge, sand disc inside — so this is a
# straight rasterise with no padding or background of its own.
#
# Deliberately a prerequisite of nothing. The PNG is committed, and an ordinary
# build must not need Inkscape and ImageMagick on the machine doing it; the
# macOS build machine has neither. Run this on the Linux tree when the artwork
# changes, and commit the result.
icon: $(ICON)

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
	@echo "  build      Build for \$$DEST"
	@echo "  test       Run the unit tests on \$$DEST"
	@echo "  icon       Re-render the app icon from graphics/gaindrive.svg"
	@echo "             (needs Inkscape and ImageMagick; not part of a build)"
	@echo "  clean      Remove the generated project and DerivedData"
