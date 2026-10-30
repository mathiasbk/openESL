# Access point settings

Most settings can be changed while the AP runs: on the **Settings** page of the [dashboard](Dashboard), through the API, or with [esl-client](esl-client). A few are set when the firmware is built.

## While it runs

| Setting | How |
|---|---|
| Default poll interval for all labels | `esl_client.py config --poll-s 60`, or `PUT /api/config {"poll_s": 60}`. `0` lets each label keep its own. |
| Poll interval for one label | `PUT /api/tags/{mac}/config {"poll_s": 300}` |
| How a label waits when the AP is gone | `PUT /api/tags/{mac}/config {"long_after_min": 1440, "long_poll_s": 3600}`: after a day without the AP a label checks only once an hour (the default). `long_after_min` 65535 turns it off. |
| Webhook for health reports | `esl_client.py config --webhook http://host/path` ([Health and webhook](Health-and-webhook)) |
| Radio transmit power | the dashboard's Settings page (Radio), `esl_client.py config --tx-power 8`, or `PUT /api/config {"tx_power": 8}`. 10 dBm by default; choose up to 20. |
| Radio channel | the dashboard's Settings page (Radio), `esl_client.py config --channel 20`, or `PUT /api/config {"channel": 20}`. 25 by default. The labels move with the AP. |
| WiFi network | the [Set up](https://mathiasbk.github.io/openESL/#setup) tab, the setup network, or `esl_client.py wifi "Network" --password ...` ([WiFi and API token](WiFi-and-API-token)) |
| API token | `esl_client.py --serial /dev/ttyACM0 wifi --new-token NEWTOKEN` |
| Name on the network | `esl_client.py wifi --hostname NAME` (gives `NAME.local`; `openesl` by default) |
| The AP's clock | set from the internet (NTP) once it is on WiFi; `esl_client.py time --set` sets it from your computer |
| Time zone | `esl_client.py config --tz "CET-1CEST,M3.5.0,M10.5.0/3"` (Norway), or `PUT /api/config {"tz": ...}`. A POSIX TZ string; UTC until set. Used for dates and times the AP fills in itself; `at` stays in UTC. |
| NTP server | `esl_client.py config --ntp no.pool.ntp.org`; `""` goes back to the one built in (`pool.ntp.org`) |

**About the poll interval:** the labels ask the AP for news every 60 seconds by default. A longer interval saves battery, but a change takes longer to show. Do not shorten it to make one label faster: it costs battery all the time. When the AP is off or gone, a label checks every 10 minutes at most, and after a day only once an hour, so a label on a shelf without its AP lasts longer; when the AP comes back, such a label can take up to an hour to show up again.

**About the transmit power:** for the labels' radio (802.15.4), the EU allows about 13 dBm, counting the antenna's gain (EN 300 328 allows 20 dBm in total, but at most 10 dBm per MHz, and the signal is about 2 MHz wide). The AP sends at 10 dBm unless you set another value. With the chip or PCB antenna of a small board, up to 13 dBm keeps within the EU limit. With an external antenna of G dBi, the limit is about 13 − G (8 dBm for a 5 dBi antenna); the antenna still helps fully when the AP listens to the labels. Outside the EU, where more may be allowed, up to 20 dBm can be set. More power reaches labels further away, but their answers do not get any stronger: for those, a better antenna on the AP, placed high with a clear view of the labels, is what helps.

**About the channel:** the labels and the AP talk on one 802.15.4 channel, 25 unless you choose another. Channel 25 sits above WiFi channel 11 and is clear of most WiFi in Europe; 15, 20 and 26 are the others clear of WiFi channels 1, 6 and 11. Change it if another device is busy on 25 (a Zigbee network, say). When you save, the AP tells each label at its next poll and moves once they all have it, at the latest after two of the longest poll intervals, so no label is left behind; **Move now** skips the wait; a label that has not heard of the new channel by then loses the AP until it is back, unless the new one is the built-in channel 25, which every label tries after it has missed the AP three times. Update every label (and a radio on WiFi) to 0.9.0 first: older firmware cannot follow a channel change, so the AP refuses to move while it hears such a label, and the Settings page says how many there are. Several APs on one site can each have their own channel, but until the radio link is paired (#260) a label that loses its AP for a while may end up with another AP that answers on 25.

## When building the firmware

`idf.py menuconfig` → "OpenESL AP": radio channel (the built-in one, 25), PAN ID, TX power (the built-in value, 10 dBm), the most labels to keep, the status page, the dashboard, OTA, the screen, and the API over USB. See [Building from source](Building-from-source) and [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#configuration).
