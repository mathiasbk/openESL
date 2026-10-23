# Scheduled prices

The AP can switch content by itself at a time you choose: new prices from Monday 07:00, or a sale that starts and one that ends it. Your computer does not need to be on then.

```sh
python3 esl_client.py sync prices.csv --at "2026-10-19 07:00"
python3 esl_client.py price 00000123456789A8 "Kaffe" "39,90" "Tilbud" --at "2026-10-20"
python3 esl_client.py schedule                  # what is waiting
python3 esl_client.py schedule-clear 00000123456789A8
```

- **The time** is local (`"2026-10-19 07:00"`, or a date for midnight), ISO with an offset, or Unix seconds. In the API it is `"at"` in Unix seconds.
- **A sale and the price after it** can be one CSV file with an `at` column: the same MAC may be in it once per time.
- **The clock:** the AP gets the time from the internet (NTP). Without it, esl-client sets the AP's clock from your computer before it schedules anything. After a restart without internet, nothing is applied until the time is set again.
- **Plan ahead:** the AP wakes the label just after the time, but it needs one poll in between. Schedule at least one poll interval ahead.
- **Room:** 256 waiting entries for all labels together, by default.

Content sent without `at` leaves scheduled entries in place. The dashboard shows how many are waiting.

Details: [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#scheduled-content).
