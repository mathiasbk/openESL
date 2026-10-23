# Dashboard

The dashboard shows the AP's state and every label, refreshed every 10 seconds.

![The dashboard](https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/web-dashboard.png)

## Opening it

- **Over WiFi:** `http://openesl.local/dashboard` (or the AP's IP address). The AP serves it itself. It asks for the API token once.
- **Over USB:** the [Dashboard](https://mathiasbk.github.io/openESL/#dashboard) tab on the web page, with the AP plugged into the computer.
- **Through the bridge:** `python3 tools/esl-client/esl_client.py --url http://openesl.local --token <token> serve`, then connect the web page to `http://localhost:8080`.

The web page on GitHub cannot reach the AP over WiFi by itself, since a page on https may not call a plain http address on your network. That is why it needs USB or the bridge.

## What it shows

- The AP: firmware, uptime, why it last restarted, radio channel, poll interval, time, schedule and WiFi.
- Four figures that also filter the table: labels, need attention, busy or late, scheduled.
- Every label: its screen as the AP draws it, last and next check-in, signal, battery, firmware, and what it is waiting for or what is wrong.

## Doing things to many labels

Tick labels in the table to:

- **Send text** with the `text` layout: the AP's address and WiFi, the label's own info, today's date, or your own four lines. Words in braces are filled in per label, such as `{mac}`, `{battery}`, `{date}` and `{ap_ip}`. With the date in it, the next days are scheduled so the label changes date at midnight by itself.
- **Send firmware** to the labels that need it ([Updating labels](Updating-labels)).
- **Forget** labels: removes them from the AP. A label that still polls comes back as a new one.

## Health and webhook

The "need attention" figure comes from the AP's health report. To get told about problems without keeping the dashboard open, see [Health and webhook](Health-and-webhook).

## The plain status page

`http://openesl.local/` is a simple page with every label and the layout files, which needs no token.
