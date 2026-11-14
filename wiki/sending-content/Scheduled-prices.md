# Scheduled prices

The AP can switch content by itself at a time you choose: new prices from Monday 07:00, or a sale that starts and one that ends it. Your computer does not need to be on then.

```sh
python3 esl_client.py sync prices.csv --at "2026-10-19 07:00"
python3 esl_client.py price 00000123456789A8 "Kaffe" "39,90" "Tilbud" --at "2026-10-20"
python3 esl_client.py price 00000123456789A8 "Kaffe" "29,90" "Fredagstilbud" --at "2026-11-20 16:00" --repeat weekly
python3 esl_client.py schedule                  # what is waiting
python3 esl_client.py schedule-clear 00000123456789A8
```

- **The time** is local (`"2026-10-19 07:00"`, or a date for midnight), ISO with an offset, or Unix seconds. In the API it is `"at"` in Unix seconds.
- **A sale and the price after it** can be one CSV file with an `at` column: the same MAC may be in it once per time.
- **The clock:** the AP gets the time from the internet (NTP). Without it, esl-client sets the AP's clock from your computer before it schedules anything. After a restart without internet, nothing is applied until the time is set again.
- **Plan ahead:** the AP wakes the label just after the time, but it needs one poll in between. Schedule at least one poll interval ahead.
- **Room:** 256 waiting entries for all labels together, by default.
- **Every day or week** (AP 0.11.0 or newer): `--repeat daily` or `--repeat weekly` with `--at`, or `"repeat": "weekly"` next to `"at"` in the API. The AP sets the content again at the same local time every day, or on the same weekday every week, and keeps that local time when daylight saving time starts or ends. A weekly offer is two weekly entries: the offer on Friday at 16:00 and the normal price on Monday at 07:00. Each recurring entry takes one place of the 256 and stays until you clear it (`schedule-clear`, or `DELETE /api/schedule/{mac}`); `schedule` shows `weekly` or `daily` next to it. A time that has passed starts it at its next time. If the AP was off over several times, the one due last is shown.

Content sent without `at` leaves scheduled entries in place. The dashboard shows how many are waiting.

Details: [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#scheduled-content).
