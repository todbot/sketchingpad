# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Zips for the web flasher, which the pages workflow attaches to each
# release:
#   circuitpython-<version>.zip  CircuitPython itself, a DFU zip
#   cp-<program>.zip             a program plus its .mpy libraries, which
#                                the page writes to CIRCUITPY over the REPL
#
#   make release
#   make clean

# Bumping this renames the zip: keep web/sketches.json in step.
CP_VERSION := 10.3.1
CP_BOARD := Seeed_XIAO_nRF52840_Sense
CP_UF2 := adafruit-circuitpython-$(CP_BOARD)-en_US-$(CP_VERSION).uf2
CP_URL := https://downloads.circuitpython.org/bin/$(CP_BOARD)/en_US/$(CP_UF2)

# circup takes the latest bundles otherwise, and a release should not
# depend on the day it was built.
BUNDLES := \
  --bundle-version adafruit/Adafruit_CircuitPython_Bundle=20261023 \
  --bundle-version adafruit/CircuitPython_Community_Bundle=20261008

RELEASE := release
BUILD := $(RELEASE)/build
NAME := circuitpython-$(CP_VERSION)
PROGRAMS := $(patsubst %/code.py,%,$(wildcard */code.py))

.PHONY: release clean

release:
	@mkdir -p $(BUILD)
	@rm -f $(RELEASE)/*.zip
	@test -f $(BUILD)/$(CP_UF2) || curl -sSfL -o $(BUILD)/$(CP_UF2) $(CP_URL)
	@python3 tools/uf2_to_hex.py $(BUILD)/$(CP_UF2) $(BUILD)/$(NAME).hex
	@adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req 0x0123 \
	  --application $(BUILD)/$(NAME).hex $(RELEASE)/$(NAME).zip > /dev/null
	@echo "$(RELEASE)/$(NAME).zip"
	@for p in $(PROGRAMS); do \
	  d=$(BUILD)/$$p; rm -rf $$d; mkdir -p $$d; \
	  cp -R $$p/ $$d/; rm -rf $$d/__pycache__; \
	  circup --path $$d --board-id $(CP_BOARD) --cpy-version $(CP_VERSION) \
	    $(BUNDLES) install --auto > $(BUILD)/$$p.log 2>&1 || \
	    { cat $(BUILD)/$$p.log; exit 1; }; \
	  echo '{"circuitpython": "$(CP_VERSION)"}' > $$d/bundle.json; \
	  (cd $$d && zip -qr - .) > $(RELEASE)/cp-$$p.zip; \
	  echo "$(RELEASE)/cp-$$p.zip"; \
	done

clean:
	rm -rf $(RELEASE)
