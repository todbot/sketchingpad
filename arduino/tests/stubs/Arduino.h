// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arduino.h -- just enough of it to build device_id_code.h on a host.
//
// That header includes <Arduino.h> for snprintf, size_t and NRF_FICR, and
// it must keep doing so: it promises to drop into any nRF52 sketch, and
// NRF_FICR only exists because Arduino.h pulls in the nRF52 headers.  So
// the test fakes Arduino.h rather than changing the header under test.
//
// The FICR device ID is a read-only hardware register on the real chip;
// here it is a plain global the test writes to sweep many device IDs.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct FakeFicr {
  uint32_t DEVICEID[2];
};

extern FakeFicr fake_ficr;
#define NRF_FICR (&fake_ficr)
