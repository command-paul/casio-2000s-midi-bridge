# casio-midi-bridge — see README.md
NAME      := casio-midi-bridge
VERSION   := 0.1.0
PREFIX    ?= $(HOME)/.local
BINDIR    := $(PREFIX)/bin
PORT_NAME ?= Casio USB MIDI
LABEL     := com.github.casio-midi-bridge
AGENTS    := $(HOME)/Library/LaunchAgents
PLIST     := $(AGENTS)/$(LABEL).plist
LOG       := $(HOME)/Library/Logs/$(NAME).log
UID_      := $(shell id -u)

APP_NAME  := Casio MIDI Bridge
APP        = $(BUILD)/$(APP_NAME).app
APP_ID    := com.github.casio-midi-bridge.app
SWIFTC    ?= swiftc
SWIFT_SRC := $(wildcard app/*.swift)

CC        ?= clang
CFLAGS    ?= -O2 -Wall -Wextra
CFLAGS    += -DVERSION=\"$(VERSION)\" -Wno-deprecated-declarations
FW_BRIDGE := -framework IOKit -framework CoreFoundation -framework CoreMIDI

BUILD := build
BINS  := $(BUILD)/$(NAME) $(BUILD)/midimon $(BUILD)/usbdesc

all: $(BINS)

$(BUILD):
	mkdir -p $@

$(BUILD)/bridge.o: src/bridge.c src/bridge.h src/usbmidi.h | $(BUILD)
	$(CC) $(CFLAGS) -c src/bridge.c -o $@

$(BUILD)/$(NAME): src/cli.c src/bridge.h $(BUILD)/bridge.o
	$(CC) $(CFLAGS) src/cli.c $(BUILD)/bridge.o -o $@ $(FW_BRIDGE)

# --- macOS app (SwiftUI front end around the same bridge core) ---
app: $(BUILD)/bridge.o $(SWIFT_SRC) src/bridge.h app/Info.plist.in app/AppIcon.icns
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	$(SWIFTC) -O -parse-as-library -import-objc-header src/bridge.h $(SWIFT_SRC) $(BUILD)/bridge.o \
	    -o "$(APP)/Contents/MacOS/$(APP_NAME)" -framework AppKit -framework SwiftUI -framework ServiceManagement $(FW_BRIDGE)
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@APP_ID@|$(APP_ID)|g' -e 's|@APP_NAME@|$(APP_NAME)|g' app/Info.plist.in > "$(APP)/Contents/Info.plist"
	cp app/AppIcon.icns "$(APP)/Contents/Resources/AppIcon.icns"
	codesign --force -s - "$(APP)"
	@echo "Built $(APP)"

# Regenerate the icon (only needed when app/icon/make-icon.swift changes).
icon: | $(BUILD)
	$(SWIFTC) -O app/icon/make-icon.swift -o $(BUILD)/make-icon -framework AppKit
	rm -rf $(BUILD)/AppIcon.iconset && mkdir -p $(BUILD)/AppIcon.iconset
	$(BUILD)/make-icon $(BUILD)/AppIcon.iconset
	iconutil -c icns $(BUILD)/AppIcon.iconset -o app/AppIcon.icns

# Copy the app to /Applications (falls back to ~/Applications) and open it.
install-app: app
	@dest=/Applications; [ -w /Applications ] || { dest=$$HOME/Applications; mkdir -p "$$dest"; }; \
	  rm -rf "$$dest/$(APP_NAME).app" && cp -R "$(APP)" "$$dest/" && echo "Installed to $$dest/$(APP_NAME).app" && open "$$dest/$(APP_NAME).app"

# Render the app window to docs/app-screenshot.png (keyboard must be connected; quits any running copy).
screenshot: app
	-pkill -x "$(APP_NAME)" 2>/dev/null; sleep 1
	mkdir -p docs
	"$(APP)/Contents/MacOS/$(APP_NAME)" --snapshot "$(CURDIR)/docs/app-screenshot.png"
	@ls -la docs/app-screenshot.png

# Zip the app for distribution (build/Casio-MIDI-Bridge-<version>.zip).
dist: app
	ditto -c -k --keepParent "$(APP)" "$(BUILD)/Casio-MIDI-Bridge-$(VERSION).zip"
	@echo "Wrote $(BUILD)/Casio-MIDI-Bridge-$(VERSION).zip"

uninstall-app:
	pkill -x "$(APP_NAME)" 2>/dev/null || true
	rm -rf "/Applications/$(APP_NAME).app" "$(HOME)/Applications/$(APP_NAME).app"

$(BUILD)/midimon: tools/midimon.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -framework CoreMIDI -framework CoreFoundation

$(BUILD)/usbdesc: tools/usbdesc.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -framework IOKit -framework CoreFoundation

$(BUILD)/test_usbmidi: tests/test_usbmidi.c src/usbmidi.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< -o $@

test: $(BUILD)/test_usbmidi
	./$(BUILD)/test_usbmidi

# Install the binary for the current user and register a LaunchAgent that keeps it running.
install: $(BUILD)/$(NAME)
	install -d "$(BINDIR)" "$(AGENTS)" "$(dir $(LOG))"
	install -m 755 $(BUILD)/$(NAME) "$(BINDIR)/$(NAME)"
	-launchctl bootout gui/$(UID_)/$(LABEL) 2>/dev/null
	sed -e 's|@BIN@|$(BINDIR)/$(NAME)|g' -e 's|@PORT_NAME@|$(PORT_NAME)|g' \
	    -e 's|@LOG@|$(LOG)|g' -e 's|@LABEL@|$(LABEL)|g' launchd/agent.plist.in > "$(PLIST)"
	launchctl bootstrap gui/$(UID_) "$(PLIST)"
	@echo "Installed and started. Try: make status"

uninstall:
	-launchctl bootout gui/$(UID_)/$(LABEL) 2>/dev/null
	rm -f "$(PLIST)" "$(BINDIR)/$(NAME)"
	@echo "Removed."

status:
	@launchctl print gui/$(UID_)/$(LABEL) 2>/dev/null | grep -E '^[[:space:]]*(state|pid) ' || echo "agent not loaded"
	@tail -n 5 "$(LOG)" 2>/dev/null || true

log:
	tail -f "$(LOG)"

restart:
	launchctl kickstart -k gui/$(UID_)/$(LABEL)

clean:
	rm -rf $(BUILD)

.PHONY: all app icon dist screenshot test install uninstall install-app uninstall-app status log restart clean
