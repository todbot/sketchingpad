# Notes: low power in CircuitPython vs Arduino on nRF52840

Question: can the CircuitPython `alarm` module give the same low-power
modes as `arduino/ts20/ble_keyboard_lowpower_ts20/`?

Short answer: **partly.** Roughly the IDLE tier is reachable. The DEEP
tier is not, and the biggest connected-mode lever is not. These are
limits of the CircuitPython nordic port, not of Python.

Everything below was read out of the CircuitPython source tree
(`~/projects/adafruit/circuitpython-claudetest/`, v10.3.0-alpha.3).
Paths are relative to that tree.

## 1. "Deep sleep" on nordic is not System OFF

This is the headline. `common_hal_alarm_enter_deep_sleep()`
(`ports/nordic/common-hal/alarm/__init__.c:241-255`) runs the *same*
`system_on_idle_until_alarm()` loop that light sleep uses, with a slower
RTC prescaler, then calls `reset_cpu()`. There is no
`sd_power_system_off()` -- grep finds no `system_off` or `SYSTEMOFF`
anywhere in `ports/nordic/` outside the vendor SDK.

The port says so itself, at
`ports/nordic/common-hal/microcontroller/Processor.c:122-123`:

> Our "deep sleep" is still actually light sleep followed by a software
> reset.

So the Arduino DEEP tier's ~1-3 uA is **unreachable**. Worse, deep sleep
here is a net loss: same current as light sleep, and you also lose all
program state and pay a **5-second full-power grace period on every
restart** (`main.c:674-676`, `CIRCUITPY_WORKFLOW_CONNECTION_SLEEP_DELAY`)
-- and deep sleep restarts the board.

It also cannot be measured over USB: `supervisor_workflow_active()`
(`supervisor/shared/workflow.c:72-92`) forces "fake sleep" whenever USB
is connected, so `common_hal_alarm_enter_deep_sleep()` is never reached
with a host attached.

The `alarm` docs' "shuts down power to nearly all of the
microcontroller including the CPU and RAM" is accurate for the ESP32
port. It is misleading for nordic.

## 2. Slave latency is not exposed -- the real gap

Arduino calls
`BLEConnection::requestConnectionParameter(interval, latency, timeout)`.
CircuitPython exposes exactly one connected-mode knob to Python:
`Connection.connection_interval`
(`shared-bindings/_bleio/Connection.c:216`).

Its setter (`ports/nordic/common-hal/_bleio/Connection.c:380-391`)
writes only `min_conn_interval`/`max_conn_interval`. `slave_latency` is
inherited from whatever the central assigned. The preferred parameters
hardcode `BLE_SLAVE_LATENCY 0` (`_bleio/Adapter.c:37`) at compile time,
and the connect handler at `_bleio/Adapter.c:272-279` can push the whole
PPCP struct back at the central, re-zeroing a latency it had offered.

Why this matters more than it sounds: slave latency lets the device skip
N connection events **while keeping a short interval**, so it stays
instantly responsive to the next touch. Raising the interval instead
buys the same average power by making *every* response slow. For a touch
surface that is the worse trade -- and it is the only trade available.

It is also capped. The SoftDevice enforces
`conn_sup_timeout * 4 > (1 + latency) * interval`; `conn_sup_timeout` is
fixed at 4000 ms (`_bleio/Adapter.c:38`) with no Python access, and the
setter does no clamping, so overshooting raises a bare
`ValueError("Invalid BLE parameter")` (`_bleio/__init__.c:35-37`).

## 3. Two smaller Arduino levers also missing

- **TX power on a connection.** `_bleio/Adapter.c:699-701` hardcodes
  `sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_CONN, handle, 0)` on
  every connect, and `Adapter.tx_power` is not in the Python locals dict
  (`shared-bindings/_bleio/Adapter.c:479-497`). `setTxPower(-4)` has no
  equivalent. Advertising TX power *is* settable per
  `start_advertising(tx_power=N)`, but is discarded the moment a central
  connects.
- **DC/DC regulator.** No `sd_power_dcdc_mode_set` or `DCDCEN` anywhere
  in `ports/nordic/`. It is never enabled and there is no API to enable
  it. Worth roughly 25-30% on its own.

## 4. What does work

**BLE survives light sleep.** `port_idle_until_interrupt()`
(`ports/nordic/supervisor/port.c:327-337`) branches to
`sd_app_evt_wait()` when the SoftDevice is enabled rather than a raw
`__WFI()`, and the alarm code contains no BLE teardown.
`RUN_BACKGROUND_TASKS` (`alarm/__init__.c:169`) keeps servicing BLE
every pass. A connected central stays connected across
`alarm.light_sleep_until_alarms()`.

**The TS20 INT pulse is caught.** This one looks fatal at first.
`PinAlarm` rejects `edge=True` outright
(`alarm/pin/PinAlarm.c:31-34`, "Cannot wake on pin edge, only level"),
and the INT line is a **~995 us active-low pulse, not a held level**
(measured; see `arduino/README.md`). But `configure_pins_for_sleep()`
arms `NRF_GPIOTE_POLARITY_HITOLO` for a `value=False` alarm
(`PinAlarm.c:148`) -- a falling-**edge** GPIOTE event -- and light sleep
calls it immediately (`PinAlarm.c:197-199`). So `value=False` does the
right thing despite what the API calls it.

**`fast_mode=False` ports exactly.** The upstream TS20 driver takes it
as a keyword-only constructor argument applied in its single startup
`reset()`, matching the Arduino requirement to set it before `begin()`.

**The LED rail P-FET is the whole ballgame anyway.** Per the power
budget in `arduino/README.md`, gating it is worth ~10-20 mA and
dominates every other technique by ~100x. In CircuitPython it is plain
`digitalio`. So a CircuitPython build is far from pointless; it just
tops out at a much higher floor.

## 5. Gotchas the code has to respect

- **At most two `PinAlarm`s.** `MAX_N_PIN_ALARMS_FOR_SENSE_FEATURE = 2`
  (`PinAlarm.c:128-138`). A third silently sets `hi_accuracy=true` on
  all of them -- dedicated GPIOTE channels with HFCLK forced on, which
  the source's own comment prices at **~10x the sleep current**. There
  is no runtime warning. Adding a wake pin later is a real regression.
- **Alarm objects must be rebuilt every pass**, since
  `light_sleep_until_alarms()` ends with `alarm_reset()`
  (`alarm/__init__.c:230`), which uninits the GPIOTE channels.
- **Never hold a `DigitalInOut` on a wake pin across the sleep call.**
  The pin has to be free for `PinAlarm` to claim it.
- **Only one `TimeAlarm`** (`alarm/time/TimeAlarm.c:68-70`).
- After a deep sleep the reconstructed `wake_alarm` is unreliable:
  `PinAlarm.value`/`.pull` are never set (`PinAlarm.c:85-99`) and
  `TimeAlarm.monotonic_time` is hardcoded to `0.0` with a TODO
  (`TimeAlarm.c:32-39`). Only `.pin` is trustworthy.
- **Brown-out reset is armed at 2.7 V** (`supervisor/port.c:141-148`,
  `NRF_POWER_POFTHR_V27`). Relevant if you plan to run a LiPo down: the
  board hard-resets rather than browning out gracefully.

## 6. Summary

| Arduino tier | CircuitPython |
|---|---|
| ACTIVE -- INT-driven, latency 0 | **Yes**, PinAlarm + light sleep |
| IDLE -- latency 20, LED rail off | **Partly**: rail off yes, latency no |
| DEEP -- SYSTEM_OFF, ~1-3 uA | **No**, and CP deep sleep is a net loss |

`ble_keyboard_lowpower/code.py` implements the two reachable tiers.
Getting the third, or real slave latency, would mean patching the C port
and building custom firmware.
