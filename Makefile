# Repository-wide conveniences. The server is built with CMake and each client
# has its own Makefile (android/, ios/); what lands here belongs to none of them.

INKSCAPE ?= inkscape
MAGICK   ?= magick

LOGO      := graphics/gaindrive.svg
PLAY_DIR  := graphics/play
PLAY_ICON := $(PLAY_DIR)/icon-512.png
PLAY_RAW  := $(PLAY_DIR)/.icon-raw.png

.PHONY: upload-web create-play-assets

upload-web:
	rsync -r html/ gaindrive-html:/var/www/gaindrive/

# The Google Play listing assets, derived from graphics/gaindrive.svg rather
# than drawn separately, so they cannot drift from the artwork or from the iOS
# icon that ios/Makefile renders the same way. A listing image can only be
# uploaded by hand through the Play Console, so this is a prerequisite of
# nothing and no build needs Inkscape or ImageMagick. The feature graphic and
# the screenshots belong here too once they exist.
create-play-assets: $(PLAY_ICON)

# 512x512 is the only size Play accepts, and it wants a full square: it applies
# its own rounded-corner mask and shadow, so the SVG's full-bleed red is already
# the right shape and nothing is padded or inset here.
#
# Rendered at 2048 and downsampled because supersampling antialiases the record
# grooves better than asking Inkscape for 512 directly.
#
# `-flatten` onto the same brand red is not cosmetic: the SVG's rectangle stops
# half a pixel short of its canvas, and a transparent edge under Play's mask
# shows as a pale hairline. The alpha channel is then put back, opaque, and the
# colour type forced, because Play asks for a 32-bit PNG — flattening alone
# would write a 24-bit one.
$(PLAY_ICON): $(LOGO)
	@mkdir -p $(PLAY_DIR)
	$(INKSCAPE) $< --export-type=png -w 2048 -h 2048 \
	 --export-filename=$(PLAY_RAW)
	$(MAGICK) $(PLAY_RAW) -background '#a31623' -flatten -resize 512x512 \
	 -alpha set -strip -define png:color-type=6 $@
	@rm -f $(PLAY_RAW)
	@echo "Wrote $@ — upload it as the app icon in the Play Console listing."
