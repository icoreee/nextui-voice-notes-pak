PAK_NAME := Voice Notes
PAK_TYPE := Tools
PLATFORMS := tg5040
VERSION ?= 0.1.0

CC ?= gcc
PKG_CONFIG ?= pkg-config
APOSTROPHE_DIR := third_party/apostrophe

SDL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_ttf SDL2_image 2>/dev/null)
SDL_LIBS ?= $(shell $(PKG_CONFIG) --libs sdl2 SDL2_ttf SDL2_image 2>/dev/null || printf '%s' '-lSDL2 -lSDL2_ttf -lSDL2_image')

CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -I$(APOSTROPHE_DIR)/include $(SDL_CFLAGS)
LDFLAGS += $(SDL_LIBS) -lm -lpthread

DIST_DIR := dist
BUILD_DIR := build
SRC_DIR := src

.PHONY: clean build release

clean:
	rm -rf "$(BUILD_DIR)" "$(DIST_DIR)"

build:
	for platform in $(PLATFORMS); do \
		pak_dir="$(BUILD_DIR)/$(PAK_TYPE)/$$platform/$(PAK_NAME).pak"; \
		platform_cflags="$(CFLAGS)"; \
		if [ "$$platform" = "tg5040" ]; then platform_cflags="$$platform_cflags -DPLATFORM_TG5040"; fi; \
		mkdir -p "$$pak_dir"; \
		cp "$(SRC_DIR)/launch.sh" "$(SRC_DIR)/pak.json" "$$pak_dir/"; \
		$(CC) $$platform_cflags "$(SRC_DIR)/voice_notes.c" -o "$$pak_dir/voice-notes.elf" $(LDFLAGS); \
		chmod +x "$$pak_dir/launch.sh" "$$pak_dir/voice-notes.elf"; \
	done

release: build
	mkdir -p "$(DIST_DIR)"
	for platform in $(PLATFORMS); do \
		zip_path="$(DIST_DIR)/NextUI-Voice-Notes-v$(VERSION)-$$platform.zip"; \
		rm -f "$$zip_path"; \
		if command -v zip >/dev/null 2>&1; then \
			(cd "$(BUILD_DIR)" && zip -qr "../$$zip_path" "$(PAK_TYPE)/$$platform/$(PAK_NAME).pak"); \
		else \
			(cd "$(BUILD_DIR)" && python3 ../scripts/create_release_zip.py "../$$zip_path" "$(PAK_TYPE)/$$platform/$(PAK_NAME).pak"); \
		fi; \
	done
	ls -lah "$(DIST_DIR)"
