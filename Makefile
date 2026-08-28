# Repository-wide conveniences. The server is built with CMake and each client
# has its own Makefile (android/, ios/); what lands here belongs to none of them.

INKSCAPE ?= inkscape
MAGICK   ?= magick

API_TOML := doc/api.toml
API_GEN  := doc/gen_api_html.py
API_HTML := html/api.html

LOGO      := graphics/gaindrive.svg
FEATURE   := graphics/play-feature.svg
PLAY_DIR  := graphics/play
PLAY_ICON := $(PLAY_DIR)/icon-512.png
PLAY_FEAT := $(PLAY_DIR)/feature-1024x500.png
PLAY_RAW  := $(PLAY_DIR)/.icon-raw.png
FEAT_RAW  := $(PLAY_DIR)/.feature-raw.png

.PHONY: help upload-web api-html create-play-assets

help:
	@echo "upload-web:          Upload web pages to server."
	@echo "api-html:            Regenerate html/api.html from doc/api.toml."
	@echo "create-play-assets:  Convert original assets to play store png files."

api-html: $(API_HTML)

# Generated but checked in, unlike the Play assets below, and the difference is
# in what consumes them: those are uploaded by hand through the Play Console,
# so nothing in the repo reads them and a binary diff is worthless. This is a
# page of the site, `upload-web` ships whatever html/ holds, and a gitignored
# api.html would publish a dead link from a fresh clone. It is also text, so
# the diff is the review — which is the only check a mistake in api.toml gets.
#
# html/docs.html is a prerequisite because the page's <head>, nav and footer
# are lifted from it rather than kept as a ninth copy, so a restyle of the site
# regenerates this page instead of leaving it behind.
$(API_HTML): $(API_TOML) $(API_GEN) html/docs.html
	python3 $(API_GEN) --out $@

upload-web: $(API_HTML)
	rsync -r html/ gaindrive-html:/var/www/gaindrive/

# The Google Play listing assets, derived from graphics/gaindrive.svg rather
# than drawn separately, so they cannot drift from the artwork or from the iOS
# icon that ios/Makefile renders the same way. A listing image can only be
# uploaded by hand through the Play Console, so this is a prerequisite of
# nothing and no build needs Inkscape or ImageMagick. The screenshots belong
# here too once they exist.
create-play-assets: $(PLAY_ICON) $(PLAY_FEAT)

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

# The banner across the top of the listing. Its layout is graphics/play-feature.svg
# rather than an ImageMagick composition here, because a banner is design work
# and belongs in a file that can be opened and moved around; that file in turn
# clips the icon artwork rather than copying it, so the two stay one set.
#
# Rendered at final size, where the icon is supersampled — the opposite choice,
# for the opposite reason. The record's grooves are 2px rings roughly 5px
# apart, and averaging a 2x render of them down beats the ring period against
# the pixel grid: the result is moiré, blotchy arcs across the disc that look
# like a compression artefact. Inkscape's own antialiasing at 1024x500 draws
# them cleanly.
#
# `-alpha off` where the icon rule sets alpha on, and that inversion is the
# specification's: Play asks for the icon as a 32-bit PNG and refuses any
# transparency in the feature graphic. Flattening onto the paper the artwork
# already sits on makes the difference invisible.
$(PLAY_FEAT): $(FEATURE)
	@mkdir -p $(PLAY_DIR)
	$(INKSCAPE) $< --export-type=png -w 1024 -h 500 \
	 --export-filename=$(FEAT_RAW)
	$(MAGICK) $(FEAT_RAW) -background '#f6efe0' -flatten -alpha off -strip $@
	@rm -f $(FEAT_RAW)
	@echo "Wrote $@ — upload it as the feature graphic in the same listing."
