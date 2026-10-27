# Access point settings

Most settings can be changed while the AP runs, through the API or [esl-client](esl-client). A few are set when the firmware is built.

## While it runs

| Setting | How |
|---|---|
| Default poll interval for all labels | `esl_client.py config --poll-s 60`, or `PUT /api/config {"poll_s": 60}`. `0` lets each label keep its own. |
| Poll interval for one label | `PUT /api/tags/{mac}/config {"poll_s": 300}` |
| Webhook for health reports | `esl_client.py config --webhook http://host/path` ([Health and webhook](Health-and-webhook)) |
| Radio transmit power | the dashboard's Access point card, `esl_client.py config --tx-power 15`, or `PUT /api/config {"tx_power": 15}`. 20 dBm by default. |
| WiFi network | the [Set up](https://mathiasbk.github.io/openESL/#setup) tab, the setup network, or `esl_client.py wifi "Network" --password ...` ([WiFi and API token](WiFi-and-API-token)) |
| API token | `esl_client.py --serial /dev/ttyACM0 wifi --new-token NEWTOKEN` |
| Name on the network | `esl_client.py wifi --hostname NAME` (gives `NAME.local`; `openesl` by default) |
| The AP's clock | set from the internet (NTP) once it is on WiFi; `esl_client.py time --set` sets it from your computer |

**About the poll interval:** the labels ask the AP for news every 60 seconds by default. A longer interval saves battery, but a change takes longer to show. Do not shorten it to make one label faster: it costs battery all the time.

**About the transmit power:** 20 dBm (100 mW) is the most the EU allows at 2.4 GHz, counting the antenna's gain. With the chip or PCB antenna of a small board, 20 dBm is fine. With an external antenna of G dBi, set at most 20 − G (15 dBm for a 5 dBi antenna). More power reaches labels further away, but their answers do not get any stronger: for those, a better antenna on the AP, placed high with a clear view of the labels, is what helps.

## When building the firmware

`idf.py menuconfig` → "OpenESL AP": radio channel (25), PAN ID, TX power (the built-in value, 20 dBm), the most labels to keep, the status page, the dashboard, OTA, the screen, and the API over USB. See [Building from source](Building-from-source) and [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#configuration).
