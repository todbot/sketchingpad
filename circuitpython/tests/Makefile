# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host tests for the CircuitPython touch layer.
#
# Stdlib unittest and nothing else, the same way arduino/tests needs only a
# compiler: a test suite that needs installing first is a test suite that
# stops being run.
#
#   make run
#   make clean

PYTHON ?= python3

.PHONY: all run clean

all: run

run:
	@$(PYTHON) -m unittest discover -s . -p '*_test.py' -v

clean:
	rm -rf __pycache__ stubs/__pycache__ .pytest_cache
