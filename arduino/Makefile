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
#   make sync     shared/ -> every sketch copy
#   make check    fail, naming names, if any copy has drifted
#   make release  every sketch -> release/<sketch>.zip, for adafruit-nrfutil
#   make web      release/ zips -> ../web/firmware, to test the flasher
#
# The sketch copies are found rather than listed: a hand-written list is
# one more thing that can drift.

SHARED := shared
HEADERS := $(notdir $(wildcard $(SHARED)/*.h))

# tests/ reads shared/ directly and tests/build holds generated files.
FIND := find . -not -path './$(SHARED)/*' -not -path './tests/*' -name

.PHONY: all sync check release web

all: check

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
SKETCHES := $(patsubst %/,%,$(dir $(shell find . -name '*.ino' -not -path './tests/*')))

release: check
	@mkdir -p $(RELEASE)
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
