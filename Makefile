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

CC        ?= clang
CFLAGS    ?= -O2 -Wall -Wextra
CFLAGS    += -DVERSION=\"$(VERSION)\" -Wno-deprecated-declarations
FW_BRIDGE := -framework IOKit -framework CoreFoundation -framework CoreMIDI

BUILD := build
BINS  := $(BUILD)/$(NAME) $(BUILD)/midimon $(BUILD)/usbdesc

all: $(BINS)

$(BUILD):
	mkdir -p $@

$(BUILD)/$(NAME): src/main.c src/usbmidi.h | $(BUILD)
	$(CC) $(CFLAGS) src/main.c -o $@ $(FW_BRIDGE)

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

.PHONY: all test install uninstall status log restart clean
