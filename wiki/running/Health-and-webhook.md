# Health and webhook

The AP keeps an eye on its labels and can tell you when something needs attention.

## What counts as a problem

| Flag | Means |
|---|---|
| `silent` | No poll for 3 × the label's interval |
| `unseen` | Known from before, but not heard from since the AP started |
| `low_battery` | Below 2500 mV (a label that reports 0 cannot measure it) |
| `draw_failed` | The AP could not draw its screen |
| `content_error` | It has content it cannot show, such as old firmware that does not take images |

## Asking for it

```sh
python3 esl_client.py health                    # exits 1 when something needs attention
curl -H "Authorization: Bearer $TOKEN" "http://openesl.local/api/health?silent_s=600&battery_mv=2400"
```

The thresholds can be changed per call. The dashboard shows the same report.

## Webhook

With a webhook set, the AP posts the same report to your URL whenever it changes (checked every 30 seconds):

```sh
python3 esl_client.py config --webhook http://homeserver:8123/api/webhook/esl
python3 esl_client.py config --webhook ""       # turn it off
```

Plain HTTP, no retries: a failed post is logged and the change goes out with the next one. A Home Assistant webhook works well as the receiver ([Home Assistant](Home-Assistant)).

## Long-term test

`esl_client.py log week.csv` writes a row per label per minute until you stop it, and `esl_client.py report week.csv` sums it up: outages, AP restarts, the longest time each label went without a poll, and the lowest battery.
