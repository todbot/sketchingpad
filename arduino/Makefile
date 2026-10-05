# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared headers: shared/ holds the only copies edited by hand.
#
# Every sketch keeps its own copy, because that is what makes a sketch
# folder self-contained -- it opens in the Arduino IDE with its headers
# as editable tabs, and zips up and sends as-is.  Nothing the Arduino
# toolchain offers gives that and a single copy at the same time; the
# alternatives and why each was rejected are in README.md.
#
# Available targets:
#
#   make sync          shared/ -> every sketch copy
#   make check         fail if any copy has drifted, naming names
#   make release       sketches -> release/<sketch>.zip (not ts20, mpr)
#   make web           release/ zips -> ../web/firmware, to test the flasher
#   make release-clean remove release/ and ../web/firmware
#   make flash <name>  compile and upload one sketch (PORT= to pick)
#   make help          this list
#
# The sketch copies are found rather than listed: a hand-written list is
# one more thing that can drift.

SHARED := shared
HEADERS := $(notdir $(wildcard $(SHARED)/*.h))

# tests/ reads shared/ directly and tests/build holds generated files.
FIND := find . -not -path './$(SHARED)/*' -not -path './tests/*' -name

.PHONY: all help sync check release release-clean web flash

all: check

# Prints the usage lines above, so there is one list to keep current.
help:
	@sed -n 's/^#   make /make /p' $(firstword $(MAKEFILE_LIST))

sync:
	@for h in $(HEADERS); do \
	  for f in `$(FIND) $$h`; do cp $(SHARED)/$$h $$f; done; \
	done
	@echo "synced $(words $(HEADERS)) headers from $(SHARED)/"

check:
	@fail=0; \
	for h in $(HEADERS); do \
	  for f in `$(FIND) $$h`; do \
	    cmp -s $(SHARED)/$$h $$f || { echo "drifted: $$f"; fail=1; }; \
	  done; \
	done; \
	if [ $$fail -ne 0 ]; then \
	  echo "edit $(SHARED)/ and run 'make -C arduino sync'"; exit 1; \
	fi; \
	echo "all copies match $(SHARED)/"

# DFU packages for adafruit-nrfutil, so a board can be flashed without the
# core, the libraries or a build.  Install steps are in README.md.
FQBN := Seeeduino:nrf52:xiaonRF52840
RELEASE := release
# ts20/ and mpr/ are the older sensor-specific sketches, not released.
SKETCHES := $(patsubst %/,%,$(dir $(shell find . -name '*.ino' \
  -not -path './tests/*' -not -path './ts20/*' -not -path './mpr/*')))

release: check
	@mkdir -p $(RELEASE)
	@rm -f $(RELEASE)/*.zip # or renamed and excluded sketches linger
	@for d in $(SKETCHES); do \
	  n=`basename $$d`; \
	  arduino-cli compile -b $(FQBN) --output-dir $(RELEASE)/build/$$n $$d \
	    > /dev/null || exit 1; \
	  cp $(RELEASE)/build/$$n/$$n.ino.zip $(RELEASE)/$$n.zip; \
	  echo "$(RELEASE)/$$n.zip"; \
	done

# Same layout the pages workflow builds from a GitHub release.  Serve with
# python3 -m http.server -d ../web; localhost counts as a secure context,
# which Web Serial requires.
WEB_FW := ../web/firmware

web:
	@mkdir -p $(WEB_FW)
	@rm -f $(WEB_FW)/*.zip
	@cp $(RELEASE)/*.zip $(WEB_FW)/
	@cd $(WEB_FW) && ls *.zip | sed 's/.*/"&"/' | paste -sd, - \
	  | sed 's/^/{"tag": "local build", "files": [/; s/$$/]}/' > index.json
	@echo "$(WEB_FW): `ls $(WEB_FW)/*.zip | wc -l | tr -d ' '` zips"

release-clean:
	rm -rf $(RELEASE) $(WEB_FW)


# The sketch name is a second goal, so give it a do-nothing rule.  Phony
# too, since most sketch names are also directories here.
ifeq (flash,$(firstword $(MAKECMDGOALS)))
  SKETCH := $(word 2,$(MAKECMDGOALS))
  $(if $(SKETCH),$(eval .PHONY: $(SKETCH))$(eval $(SKETCH):;@:))
endif

# arduino-cli names the board from its USB VID/PID on any OS, Sense too.
# Recursive, so it only runs when flash expands it.
PORT ?= $(shell arduino-cli board list | awk '/xiaonRF52840/ {print $$1; exit}')

# The bootloader comes back on the same port name, so arduino-cli's wait
# for a new one always times out.  compile -u can't take this flag.
# If uploads fail without the wait: make flash <sketch> NOWAIT=
NOWAIT := --upload-property upload.wait_for_upload_port=false

flash:
	@test -n "$(SKETCH)" || { echo "usage: make flash <sketch>"; exit 1; }
	@test -n "$(PORT)" || { echo "no XIAO found, set PORT="; exit 1; }
	@d=`find . -name $(SKETCH).ino -not -path './tests/*'`; \
	test -n "$$d" || { echo "no sketch named $(SKETCH)"; exit 1; }; \
	arduino-cli compile -b $(FQBN) `dirname $$d` && \
	arduino-cli upload -b $(FQBN) -p $(PORT) $(NOWAIT) `dirname $$d`
