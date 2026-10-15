# Power budget (requirement T6)

T6: a tag must last at least one year on its original batteries with a poll every 60 s and one display update a day, and should aim for half the budget to leave room for cold and ageing. This page works out where the tag is against that. The **timing** is from the firmware and from measurements on the jig tag; the **currents** are estimates, since there is no public ZBS243 datasheet and nothing has been measured with a current meter yet (phase 6). Every value is a parameter of `tools/power-budget/power_budget.py`, so the measured figures can be put in when they exist.

## Result

With the estimates below, a tag that hears the AP averages about **4.6 µA**. T6 allows 50 µA for one CR2450 (25 µA with the margin) and 99 µA for two in parallel, so the requirement holds with a factor of 5–10 to spare. The 1.54" tags hold one CR2450 and the 2.9" tags two, so the calculated life is 9 years for the small tags and 16 for the large ones (a bit less there, since the larger panel draws more during its daily refresh), which in practice means the shelf life of the cells (about 10 years) sets the limit, not the tag.

```
$ python3 tools/power-budget/power_budget.py --cells 1
1 x CR2450, 70 % of 620 mAh usable; poll every 60 s, 1 update(s) a day
case               polls/day    sleep    polls  display  average  years
AP answers              1440   3.00uA   1.33uA   0.26uA   4.59uA    9.3
AP gone all day          148   3.00uA   0.90uA   0.00uA   3.90uA   10.8
T6 budget (1 year): 50 uA average, half of it 25 uA; self-discharge adds 0.7 uA
Sleep current that still keeps half the budget: 22 uA
```

**Sleep current decides it.** Two thirds of the average is sleep, and a tag that sleeps at 22 µA instead of 3 µA would still just meet T6 with margin on one cell. That is the number to watch in phase 6 (risk R3): a pin that leaks through a pull-up, a panel that is not put into deep sleep, or the radio left on would each cost far more than all the polls together.

A pessimistic case (sleep 10 µA, radio 20 mA, 15 ms to wake, 10 ms to the reply, panel 6 mA) gives 14.7 µA and 3.2 years on one cell, still inside the margin.

| What changes | Average (1 × CR2450) | Life |
|---|---|---|
| The defaults above | 4.6 µA | 9.3 years |
| Poll every 30 s | 5.9 µA | 7.5 years |
| Poll every 300 s | 3.5 µA | 11.7 years |
| 24 display updates a day | 10.6 µA | 4.4 years |
| Pessimistic (see above) | 14.7 µA | 3.2 years |

Display updates are cheap once a day but add up: each one keeps the core awake for the whole 2.9 s refresh (the firmware waits on BUSY instead of sleeping), about 22 mC, or as much as 280 polls. Letting the core sleep while the panel refreshes would cut that by more than half if frequent updates ever matter.

## The model

Per day: sleep current for the whole day, plus the charge of each POLL, plus the charge of each display update.

- **POLL, answered:** wake-up (clock, radio on, frame built) at core current, then radio on until the reply is in. `5 ms × 4 mA + 5 ms × 12 mA = 0.08 mC`.
- **POLL, not answered:** wake-up, two full listening windows (the POLL and its one retry) at radio current, and the random pause between them at core current. `5 × 4 + 2 × 20 × 12 + 6 × 4 = 0.52 mC`.
- **Without an AP** the tag polls 3 times at 60 s, then doubles the interval up to 600 s (T11), so 148 polls a day.
- **Display update:** init and image transfer plus the 2.9 s refresh, at core plus panel current. `3.2 s × (4 + 3) mA = 22 mC`.
- **Battery:** usable capacity is 70 % of nominal, to cover the tag's cut-off voltage above the cell's rated 2.0 V, cold, and the voltage sag when the radio draws its peaks from a coin cell. Self-discharge of 1 % a year is added as a constant current.
- Not counted: layout downloads (about 80 ms of radio once per file), CONFIG messages, the "No AP found" screen (drawn once after 2 hours without the AP), and retries when the AP is there but a reply is lost. All of them are rare.

## The figures and where they come from

| Value | Used | Source |
|---|---|---|
| Poll interval | 60 s | `CFG_POLL_S`; measured on the jig tag: a POLL every ~60 s, also after deep sleep (phase 3 and 5) |
| Backoff without the AP | 3 misses, then doubling to 600 s | `CFG_BACKOFF_AFTER`, `CFG_MAX_POLL_S`; seen on hardware in phase 5 ("AP gone for 5 min") |
| Listening window | 20 ms, up to 2 per POLL | `CFG_RX_TIMEOUT_MS`, one retry after 2–10 ms |
| Time to the reply | 5 ms | the AP answers a frame in under 40 µs (`ap/README.md`, layout transfer timing); the rest is air time and the tag's turnaround. Not measured on the tag side |
| Wake-up time | 5 ms | estimate; the deep-sleep wake is a jump back into the code, not a reboot (`kildelogg.md`) |
| Display refresh | 2.9 s BUSY | measured on the 1.54" panel (`kildelogg.md`); init and transfer 0.3 s estimated |
| Sleep current | 3 µA | **estimate**: sleep timer, retained XRAM and the panel's deep sleep (typically under 1 µA for this panel family) |
| Core running | 4 mA | **estimate** for an 8051 at 16 MHz |
| Radio on | 12 mA | **estimate**, typical for a 2.4 GHz 802.15.4 radio in RX or TX at 0 dBm |
| Panel refresh | 3 mA | **estimate** for a 1.54" panel's charge pumps during a refresh |
| Battery | CR2450, 620 mAh nominal | Panasonic CR2450 data (to 2.0 V at 0.2 mA). The 1.54" tags hold one cell, the 2.9" tags two in parallel (checked by Mathias) |

## What phase 6 should measure

1. **Sleep current**, with the panel in deep sleep and the radio off, over a whole sleep period. If it is above about 20 µA on one cell, find the leak before anything else.
2. **Charge per POLL** (current × time over the awake period), answered and not answered, which replaces wake-up time, reply time and the radio and core currents at once.
3. **Charge per display update.**
4. **Battery voltage over time** once T8 works (issue #4), to see the real usable capacity.

Then run the tool with the measured values, for example `--sleep-ua 2.1 --wake-ms 3 --radio-ma 9.5`, and replace the estimates in the table above.
