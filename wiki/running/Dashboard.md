# Dashboard

The dashboard shows the AP's state and every label, refreshed every 10 seconds.

![The dashboard](https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/web-dashboard.png)

## Opening it

- **Over WiFi:** `http://openesl.local/dashboard` (or the AP's IP address). The AP serves it itself. It asks for the API token once. An AP with room for it also has the Design tab there, with the [label designer](Label-designer#getting-it-onto-the-ap) working on the layouts stored on that AP.
- **Over USB:** the [Dashboard](https://mathiasbk.github.io/openESL/#dashboard) tab on the web page, with the AP plugged into the computer.
- **Through the bridge (for developers):** `python3 tools/esl-client/esl_client.py --url http://openesl.local --token <token> serve`, then choose **Bridge (advanced)** on the web page and connect to `http://localhost:8080`.

The web page on GitHub cannot call the AP over WiFi by itself, since a page on https may not call a plain http address on your network. That is why it uses USB, or opens the AP's own page.

When the designer sends a layout file with **Open my AP**, it shows up at the top of the AP's own dashboard, to check and save. See [Getting layouts onto the AP](Getting-layouts-onto-the-AP).

## What it shows

- The AP: firmware, uptime, why it last restarted, radio channel, poll interval, time, schedule and WiFi.
- Four figures that also filter the table: labels, need attention, busy or late, scheduled.
- Every label: its screen as the AP draws it, last and next check-in, signal, battery, firmware, and what it is waiting for or what is wrong.

## Doing things to many labels

Tick labels in the table to:

- **Send text** with the `text` layout: the AP's address and WiFi, the label's own info, today's date, or your own four lines. Words in braces are filled in per label, such as `{mac}`, `{battery}`, `{date}` and `{ap_ip}`. With the date in it, the next days are scheduled so the label changes date at midnight by itself.
- **Send firmware** to the labels that need it ([Updating labels](Updating-labels)).
- **Forget** labels: removes them from the AP. A label that still polls comes back as a new one.

## Settings

**Settings** (next to **Tags and AP** at the top of the dashboard) changes the AP while it runs, without esl-client. The menu on the left has its groups, one shown at a time:

- **General:** the AP's name (`NAME.local`), the time zone (for dates and local times the AP fills in itself) and how often the labels check for news (the poll interval for all labels). **Save** and **Undo** show up once something differs from what the AP has, and save the whole group at once. **Set it from this computer** shows when the AP's clock is unknown or wrong, for an AP that cannot reach a time server.
- **Network:** the WiFi network the AP is on (**Change network …** to join another), the webhook where the AP posts its health report ([Health and webhook](Health-and-webhook)), and the time server (NTP).
- **Radio:** the radio's transmit power, with the EU limit for each kind of antenna ([Access point settings](Access-point-settings)). Only on an AP with a radio.
- **Security:** a new API token. This page uses it at once; other apps that call the AP need it too.
- **Backup:** download a backup file, or put one back ([Backup and restore](Backup-and-restore)).

Every AP board has the Settings page, on the AP's own dashboard and over USB. The time zone and NTP server need AP firmware newer than 0.8.2; the rest works on older firmware too ([What each board has](Access-point-boards#what-each-board-has)).

## Health and webhook

The "need attention" figure comes from the AP's health report. To get told about problems without keeping the dashboard open, see [Health and webhook](Health-and-webhook).

## The plain status page

`http://openesl.local/` is a simple page with every label and the layout files, which needs no token.
