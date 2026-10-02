# Simple BLE multi-user messaging on nRF52840

## Connectionless advertising broadcast

Every node advertises a small payload and simultaneously scans for
everyone else's. No pairing, no connections, no connection-slot limit,
unlimited participants, symmetric in both directions.

## Examples to crib from

Paths relative to `libraries/Bluefruit52Lib/examples/` in either 
the Seeed or the Adafruit nRF52 core:

- `Projects/rssi_proximity/` -- the right mechanism: custom advertising
  payload plus scanner filtering for network isolation. Caveat: asymmetric w/
   one central and many peripherals. 
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
The `Projects/rssi_proximity_peripheral` example spends it as: 

    flags 3 + TX power 3 + 128-bit UUID 18 = 24, leaving 7. 

That is why the example puts the device name in the scan response instead. 
Its own comment says "Not enough room in the advertising packet for name".

The 18 bytes of UUID is just a network ID. Manufacturer Specific
Data does the same job far cheaper, and `filterMSD(cid)` gives identical
isolation:

    31 - 3 (flags) - 2 (MSD len+type) - 2 (company ID) = 24 bytes user data

(21 if TX power is kept.) 
Plenty for a device ID, a sequence number and a few payload bytes.

The example already contains this as a commented-out alternative, along
with the caveat, quoting the SIG on company ID `0xFFFF`:

> "This value may be used in the internal and interoperability tests
> before a Company ID has been assigned. This value shall not be used in
> shipping end products."

Fine for our own devices; needs a real CID for anything shipped.

### Note: Advertising is unacknowledged, think UDP

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
That is the same hash behind the color code `ble_keyboard_simple_ts20` shows,
so a peer logged as `a3f1` can be matched to the board on the bench
lighting `RMGGB`. The startup banner prints both.
