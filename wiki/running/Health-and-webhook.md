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

## Lost radio

On an ESP32-S3 AP the labels' radio is an ESP32-C6, [on a cable](ESP32-S3-and-C6) or [over WiFi](ESP32-S3-with-wireless-C6). When the S3 has had no contact with a C6 for 30 seconds (over WiFi up to 2 minutes, when the C6 just loses power), the report lists it under `radio_problems` as `radio_lost`, with how long it has been gone, and `ok` is false. Until it answers again the labels it serves hear nothing from the AP. A webhook gets the report both when the radio is lost and when it is back. The [dashboard](Dashboard) shows it in red at the top, and [Home Assistant](Home-Assistant) as a repair. Firmware from before this sends no `radio_problems`.

```json
"radio_problems": [{"radio": 0, "id": "00001A2B3C4D5E6F", "via": "uart", "problems": ["radio_lost"], "lost_s": 300}],
"radio_lost": 1
```

## Labels of other APs

From 0.11.0 an AP does not answer the labels of another AP ([Why labels stay connected](Why-labels-stay-connected#more-than-one-ap)). When such labels poll it, the report lists their AP under `other_aps`: its id, how many of its labels were heard lately, and whether they have been without their own AP for over an hour (`lost_long`). The dashboard says so under the health line. To take them over, choose that AP under **Settings → Radio → Labels of other APs**, or `esl_client.py config --adopt 1A2B3C4D`.

```json
"other_aps": [{"ap_id": "1A2B3C4D", "tags": 3, "polls": 12, "seen_s_ago": 40, "lost_long": true}]
```

**An old radio over WiFi:** a C6 over WiFi with firmware from before 0.11.0 cannot pass the AP id on to the labels. This AP's own labels then show *settings pending* until the radio is updated, and the radio also answers the labels of other APs from its own memory, so they never get their settings from their own AP. The report lists such a radio under `old_radios` (it does not make `ok` false), and the dashboard warns about it at the top. Update it to the same version as the AP ([ESP32-S3 with wireless C6](ESP32-S3-with-wireless-C6)).

```json
"old_radios": [{"radio": 0, "id": "00001A2B3C4D5E6F", "fw": 1002}]
```

## Asking for it

```sh
python3 esl_client.py health                    # exits 1 when something needs attention
curl -H "Authorization: Bearer $TOKEN" "http://openesl.local/api/health?silent_s=600&battery_mv=2400"
```

The thresholds can be changed per call. The dashboard shows the same report.

## Webhook

With a webhook set, the AP posts the same report to your URL whenever it changes, including when a radio is lost or back (checked every 30 seconds):

```sh
python3 esl_client.py config --webhook http://homeserver:8123/api/webhook/esl
python3 esl_client.py config --webhook ""       # turn it off
```

Plain HTTP, no retries: a failed post is logged and the change goes out with the next one. A Home Assistant webhook works well as the receiver ([Home Assistant](Home-Assistant)).

## Long-term test

`esl_client.py log week.csv` writes a row per label per minute until you stop it, and `esl_client.py report week.csv` sums it up: outages, AP restarts, the longest time each label went without a poll, and the lowest battery.

`esl_client.py battery week.csv --html battery.html` reads the same log for the battery: when each label's reading first dropped a step, how fast it falls and how many days are left at that rate, plus a rough charge estimate from the power budget. The 2.9" only reports 100 mV steps below 2.6 V, so a fresh battery shows 2600 for most of its life; the 1.54" cannot measure its battery and gets the charge estimate only. The page has a chart per label.
