# Notes: simple BLE multi-user messaging on nRF52840

Question: a few bytes plus a device ID, shared between several units.
Are there Bluefruit examples? Is BLE Mesh an option?


## Connectionless advertising broadcast

Every node advertises a small payload and simultaneously scans for
everyone else's. No pairing, no connections, no connection-slot limit,
unlimited participants, symmetric in both directions.

## API is present in both installed cores

Checked `BLEScanner.h` in `Seeeduino:nrf52` 1.1.13 (what this project
builds against) and `adafruit:nrf52` 1.7.0. Identical signatures at the
same line numbers:

- `void filterMSD(uint16_t manuf_id)` (line 62)
- `void filterUuid(BLEUuid ...)` (lines 65-69)
- `void filterRssi(int8_t min_rssi)` (line 61)
- `uint8_t parseReportByType(const ble_gap_evt_adv_report_t* report,
  uint8_t type, uint8_t* buf, uint8_t bufsize = 0)` (line 86)

So anything below works on the XIAO nRF52840 target as-is. (This matters
only because the Seeed core forks an older Adafruit core; for the
scanner API it has not drifted.)

## Examples to crib from

Paths relative to `libraries/Bluefruit52Lib/examples/` in either core:

- `Projects/rssi_proximity/` -- the right mechanism: custom advertising
  payload plus scanner filtering for network isolation. Caveat: it is
  one central and many peripherals, so it is asymmetric, one-way, and
  split across two sketches. Both halves need merging into one sketch
  where each node advertises *and* scans.
- `DualRoles/dual_bleuart/` -- proves the dual role works:
  `Bluefruit.begin(1, 1)` (peripheral count, central count) at line 43,
  then `Bluefruit.Scanner.start(0)` and `Bluefruit.Advertising.start(0)`
  (0 = never stop).
- `Central/central_scan_advanced/` and
  `Projects/rssi_proximity/rssi_proximity_central/` (lines 288-323) --
  receive-side extraction with `parseReportByType()`.
- `Peripheral/adv_advanced/` -- payload construction.

## Payload budget

The advertising packet is 31 bytes total.
`rssi_proximity_peripheral` spends it as: flags 3 + TX power 3 +
128-bit UUID 18 = 24, leaving 7. That is why the example puts the device
name in the scan response instead -- its own comment says "Not enough
room in the advertising packet for name".

The 18 bytes of UUID buy nothing but a network ID. Manufacturer Specific
Data does the same job far cheaper, and `filterMSD(cid)` gives identical
isolation:

    31 - 3 (flags) - 2 (MSD len+type) - 2 (company ID) = 24 bytes user data

(21 if TX power is kept.) Plenty for a device ID, a sequence number and
a few payload bytes.

The example already contains this as a commented-out alternative, along
with the caveat, quoting the SIG on company ID `0xFFFF`:

> "This value may be used in the internal and interoperability tests
> before a Company ID has been assigned. This value shall not be used in
> shipping end products."

Fine for our own devices; needs a real CID for anything shipped.

### Note: Advertising is unacknowledged

No retries, no delivery guarantee. Include a per-device sequence number
and repeat each message over several advertising intervals, so receivers
can dedupe and tolerate loss.


## Implementation

`arduino/ble_broadcast/` is the symmetric advertise-and-scan sketch:
device ID, sequence number, 8 payload bytes, receive callback with
per-peer dedupe. Type a line into the serial monitor and it appears on
every other node in range.

The 16-bit on-air device ID comes from `deviceIdShort()` in
`device_id_code.h`, which hashes the chip's factory `NRF_FICR->DEVICEID`.
That is the same hash behind the color code `ble_keyboard_simple` shows,
so a peer logged as `a3f1` can be matched to the board on the bench
lighting `RMGGB`. The startup banner prints both.


## BLE Mesh: no

`Bluefruit52Lib/src/` is GAP/GATT only -- `BLEAdvertising`, `BLEScanner`,
`BLECentral`, `BLEPeriph`, `BLEService`, and so on. The only occurrences
of "mesh" anywhere in the library are SIG UUID *constants* in
`BLEUuid.h` (`UUID16_SVC_MESH_PROVISIONING` 0x1827, `..._MESH_PROXY`
0x1828 and their characteristics) -- assigned numbers, not a stack.

SIG BLE Mesh needs a separate stack (nRF5 SDK for Mesh, or
Zephyr/nRF Connect SDK); it is not reachable from the Arduino core on
top of SoftDevice S140. It also solves a problem we do not have --
multi-hop relay across a building, paid for with provisioning,
addressing and key management.
