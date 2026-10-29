# Power budget (requirement T6)

T6: a tag must last at least one year on its original batteries with a poll every 60 s and one display update a day, and should aim for half the budget to leave room for cold and ageing. This page works out where the tag is against that. The **timing** is from the firmware and from measurements on the jig tag; the **currents** are estimates, since there is no public ZBS243 datasheet and nothing has been measured with a current meter yet. Every value is a parameter of `tools/power-budget/power_budget.py`, and "Measuring a tag" below shows how to log a tag's real current and turn the log into those parameters with `tools/power-budget/measure.py`.

## Result

With the estimates below, a tag that hears the AP averages about **4.5 µA**. T6 allows 50 µA for one CR2450 (25 µA with the margin) and 99 µA for two in parallel, so the requirement holds with a factor of 5–10 to spare. The 1.54" tags hold one CR2450 and the 2.9" tags two, so the calculated life is 9 years for the small tags and 16 for the large ones (a bit less there, since the larger panel draws more during its daily refresh), which in practice means the shelf life of the cells (about 10 years) sets the limit, not the tag.

```
$ python3 tools/power-budget/power_budget.py --cells 1
1 x CR2450, 70 % of 620 mAh usable; poll every 60 s, 1 update(s) a day
case               polls/day    sleep    polls  display  average  years
AP answers              1440   3.00uA   1.33uA   0.12uA   4.46uA    9.6
AP gone all day          148   3.00uA   0.90uA   0.00uA   3.90uA   10.8
AP gone for days          24   3.00uA   0.15uA   0.00uA   3.15uA   12.9
T6 budget (1 year): 50 uA average, half of it 25 uA; self-discharge adds 0.7 uA
Sleep current that still keeps half the budget: 23 uA
```

**Sleep current decides it.** Two thirds of the average is sleep, and a tag that sleeps at 23 µA instead of 3 µA would still just meet T6 with margin on one cell. That is the number to measure first (risk R3): a pin that leaks through a pull-up, a panel that is not put into deep sleep, or the radio left on would each cost far more than all the polls together.

A pessimistic case (sleep 10 µA, radio 20 mA, 15 ms to wake, 10 ms to the reply, panel 6 mA) gives 14.7 µA and 3.2 years on one cell, still inside the margin.

| What changes | Average (1 × CR2450) | Life |
|---|---|---|
| The defaults above | 4.5 µA | 9.6 years |
| Poll every 30 s | 5.8 µA | 7.6 years |
| Poll every 120 s | 3.8 µA | 11.0 years |
| Poll every 300 s | 3.4 µA | 12.1 years |
| Poll every 600 s | 3.3 µA | 12.5 years |
| 24 display updates a day | 7.3 µA | 6.2 years |
| 24 updates, core awake during the refresh (before 0.9) | 10.6 µA | 4.4 years |
| Pessimistic (see above) | 14.6 µA | 3.2 years |

**A longer interval helps less than it looks.** Going from 60 s to 300 s saves about a quarter, but beyond that the sleep current is nearly all that is left, and with the "AP gone" backoff the floor is about 3.9 µA anyway, 3.2 µA once the AP has been gone for a day. A longer interval also makes a price sent with `PUT /api/tags` take up to that long to show (requirement G1 assumes 60 s); prices scheduled with `at` are not delayed, since the AP tells the tag to wake just after they are due. The interval is set for all tags at once in the dashboard (the Access point card), with `PUT /api/config` (`esl_client.py config --poll-s 300`) or per tag with `PUT /api/tags/{mac}/config`, and reaches each tag at its next poll; no reflash is needed.

Display updates are cheap once a day but add up. Since 0.9 the core sleeps while the panel refreshes, waking once a second to check BUSY (#261), so an update costs about 10 mC instead of 22 mC; before, the core waited awake through the whole 2.9 s. The start screen at power-on is still drawn awake, since the sleep timer is set up with the radio.

**When the AP is gone for long**, a tag keeps polling at the backoff's 600 s, which is 148 polls a day that each listen twice. After a day without an answer (`long_after_min`, 1440 min) the tag polls only once an hour (`long_poll_s`, 3600 s) until it hears the AP again (#261). That matters for tags on a shelf or in a store room with the AP switched off: 3.2 µA instead of 3.9 µA, two more years on one cell. The cost is that a tag finds a returned AP up to an hour late. Both can be set per tag with `PUT /api/tags/{mac}/config`, like `max_poll_s`, and `long_after_min` 65535 turns it off.

## The model

Per day: sleep current for the whole day, plus the charge of each POLL, plus the charge of each display update.

- **POLL, answered:** wake-up (clock, radio on, frame built) at core current, then radio on until the reply is in. `5 ms × 4 mA + 5 ms × 12 mA = 0.08 mC`.
- **POLL, not answered:** wake-up, two full listening windows (the POLL and its one retry) at radio current, and the random pause between them at core current. `5 × 4 + 2 × 20 × 12 + 6 × 4 = 0.52 mC`.
- **Without an AP** the tag polls 3 times at 60 s, then doubles the interval up to 600 s (T11), so 148 polls a day.
- **Without an AP for a day** the tag polls once each `long_poll_s` (3600 s), 24 polls a day.
- **Display update:** init and image transfer at core plus panel current, then the 2.9 s refresh at panel current with the core asleep. `0.3 s × (4 + 3) mA + 2.9 s × 3 mA = 10.8 mC` (`--doze 0` for the core awake: 22 mC).
- **Battery:** usable capacity is 70 % of nominal, to cover the tag's cut-off voltage above the cell's rated 2.0 V, cold, and the voltage sag when the radio draws its peaks from a coin cell. Self-discharge of 1 % a year is added as a constant current.
- Not counted: layout downloads (about 80 ms of radio once per file), CONFIG messages, the "No AP found" screen (drawn once after 2 hours without the AP), retries when the AP is there but a reply is lost, the once-a-second wake-ups to check BUSY during a refresh, and the battery reading, which wakes the panel's analog part every 4 hours (up to ~0.8 s; on the 1.54" it stops after the first step because the reading is 0; once an hour before 0.9). All of them are rare.

## The figures and where they come from

| Value | Used | Source |
|---|---|---|
| Poll interval | 60 s | `CFG_POLL_S`; measured on the jig tag: a POLL every ~60 s, also after deep sleep (phase 3 and 5) |
| Backoff without the AP | 3 misses, then doubling to 600 s | `CFG_BACKOFF_AFTER`, `CFG_MAX_POLL_S`; seen on hardware in phase 5 ("AP gone for 5 min") |
| Long backoff | 3600 s after 1440 min without the AP | `CFG_LONG_POLL_S`, `CFG_LONG_AFTER_MIN` (#261); not yet seen on hardware |
| Listening window | 20 ms, up to 2 per POLL | `CFG_RX_TIMEOUT_MS`, one retry after 2–10 ms |
| Time to the reply | 5 ms | the AP answers a frame in under 40 µs (`ap/README.md`, layout transfer timing); the rest is air time and the tag's turnaround. Not measured on the tag side |
| Wake-up time | 5 ms | estimate; the deep-sleep wake is a jump back into the code, not a reboot (`kildelogg.md`) |
| Display refresh | 2.9 s BUSY | measured on the 1.54" panel (`kildelogg.md`); init and transfer 0.3 s estimated |
| Sleep current | 3 µA | **estimate**: sleep timer, retained XRAM and the panel's deep sleep (typically under 1 µA for this panel family) |
| Core running | 4 mA | **estimate** for an 8051 at 16 MHz |
| Radio on | 12 mA | **estimate**, typical for a 2.4 GHz 802.15.4 radio in RX or TX at 0 dBm |
| Panel refresh | 3 mA | **estimate** for a 1.54" panel's charge pumps during a refresh |
| Battery | CR2450, 620 mAh nominal | Panasonic CR2450 data (to 2.0 V at 0.2 mA). The 1.54" tags hold one cell, the 2.9" tags two in parallel (checked by Mathias) |

## Measuring a tag

What to measure, in order of what it decides:

1. **Sleep current**, with the panel in deep sleep and the radio off, over a whole sleep period. If it is above about 20 µA on one cell, find the leak before anything else.
2. **Charge per POLL** (current × time over the awake period), which replaces wake-up time, reply time and the radio and core currents at once.
3. **Charge per display update.**
4. **Battery voltage over time**: the AP keeps each tag's last reading (`battery_mv` in `GET /api/tags`), so a tag left running for months shows the real usable capacity.

The current spans three decades, from a few µA asleep to 10–20 mA with the radio on, so a plain multimeter only gets the sleep current (on its µA range, read after the tag has gone back to sleep, and with the meter's burden voltage in mind: on a coin cell it can brown the tag out when the radio starts). A logging meter with automatic range switching gets all of it. The Nordic Power Profiler Kit II (PPK2) does, and is what `measure.py` reads directly:

1. Take the cell out and power the tag from the PPK2 in **source mode** at 3.0 V (one CR2450; the 2.9" tags hold two in parallel, still 3.0 V), from its VOUT and GND to the cell's + and −. Use short wires; the radio's current steps are fast.
2. In nRF Connect's Power Profiler, sample at 100 kHz (10 µs) and log for at least ten minutes with the tag on its AP, so there are around ten POLLs at 60 s. Send the tag new content once during the log (in the dashboard, or `esl_client.py`) to have a display update in it.
3. Select the whole log and **Export** it as CSV (time and current; the digital channels are not needed).
4. Run `python3 tools/power-budget/measure.py export.csv --cells 1` (2 for a 2.9" tag).

The script finds each wake-up (the current above 100 µA, `--threshold-ua`), calls the short ones POLLs and the long ones display updates or downloads, and reports the sleep current between them, the median charge and length of a POLL, the interval, and each long wake-up. It ends with the `power_budget.py` command line for the measured figures and that table:

```
Log: 601.0 s, average 22.16 uA
Sleep current: 3.00 uA (between wake-ups)
POLLs: 9, each 73 uC over 12.0 ms (median), peak 6.0 mA, every 60 s
Long wake-up at 241.0 s: 3.21 s, 10.86 mC, peak 7.0 mA (a display update, or an image or firmware download)

As measured figures: python3 tools/power-budget/power_budget.py --sleep-ua 3.00 --poll-mc 0.0726 --update-mc 10.86
```

(that output is from the made-up log in `tools/power-budget/test_measure.py`, not a real tag; the average is high because a ten-minute log holds a whole display update). Any other logger works if it saves two columns, time and current: say their units with `--time-unit` and `--current-unit` (s, ms or us; A, mA, uA or nA) when the header does not.

When there are real figures, put them in the tables above and in the defaults of `power_budget.py`, with where they came from.
