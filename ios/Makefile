# Convenience targets for the iOS client. The server itself is built with
# CMake — see CLAUDE.md — and this Makefile only runs on the macOS build
# machine, which has its own checkout.

XCODEGEN := xcodegen
XCODEBUILD := xcodebuild
PROJECT := GainDrive.xcodeproj
SCHEME := GainDrive

# Override on the command line for a different simulator, e.g.
#   make test DEST='platform=iOS Simulator,name=iPad Pro 13-inch (M4)'
DEST ?= platform=iOS Simulator,name=iPhone 16

.PHONY: generate open build test clean help

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

# The generated project and its build products only; project.yml is the thing
# that is kept.
clean:
	rm -rf $(PROJECT) DerivedData

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
	@echo "  clean      Remove the generated project and DerivedData"
