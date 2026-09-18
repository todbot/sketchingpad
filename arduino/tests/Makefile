# Host tests for the dependency-free headers in the sketch folders.
#
# led_anim.h and anim_wire.h depend only on <stdint.h>, so they build and
# run natively -- no board, no radio, no BLE.  Run these before flashing:
# a chunk-reassembly bug is miserable to diagnose over the air and trivial
# to catch here.
#
#   make run      build and run everything
#   make clean
#
# The headers are copied per sketch (same convention as device_id_code.h),
# so INCLUDES names the copy under test.

CXX      ?= c++
# Sanitizers are the point of building these natively: an out-of-bounds
# write in the reassembler is silent on the nRF52 and fatal here.
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1 -g \
            -fsanitize=address,undefined -fno-omit-frame-pointer
INCLUDES  = -I../ble_broadcast_anim -I../hwtest_touch -Istubs
BUILD     = build

SRCS := $(wildcard *_test.cpp)
BINS := $(patsubst %.cpp,$(BUILD)/%,$(SRCS))

.PHONY: all run clean
all: $(BINS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%: %.cpp test_util.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

run: $(BINS)
	@fail=0; for t in $(BINS); do ./$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "\nall suites passed"; \
	 else echo "\nSUITES FAILED"; fi; exit $$fail

clean:
	rm -rf $(BUILD)
