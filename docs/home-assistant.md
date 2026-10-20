# Home Assistant: 3D printer status on a tag

Home Assistant can be a client of the AP's API like any other: it sends the text straight to the AP with `PUT /api/tags/{mac}` ([openapi.yaml](openapi.yaml)) and the built-in layout `text`, four lines of free text. This example shows the status of a 3D printer whose data Home Assistant already has as entities. The same pattern works for any other sensor.

Replace these before you paste:
- `openesl.local`: the name of your AP on the network (or its IP)
- `00000123456789A8`: the full MAC of the tag (16 hex digits), from `GET /api/tags` or the dashboard
- `sensor.printer_*`: your own entities for status, progress (%), time remaining (minutes) and file name

## 1. `secrets.yaml`

```yaml
esl_auth: "Bearer YOUR_API_TOKEN"
```

## 2. `configuration.yaml`

```yaml
rest_command:
  esl_tag_text:
    url: "http://openesl.local/api/tags/{{ mac }}"
    method: PUT
    headers:
      Authorization: !secret esl_auth
    content_type: "application/json"
    payload: >-
      {"layout":"{{ layout | default('text') }}","fields":{{ fields | to_json }}}
```

Restart Home Assistant (or reload the YAML) after this.

## 3. Automation

```yaml
alias: 3D printer to ESL tag
mode: queued
max: 2
triggers:
  # At once when the status changes (start, pause, finished, error)
  - trigger: state
    entity_id: sensor.printer_status
  # Every 5 minutes while printing
  - trigger: time_pattern
    minutes: "/5"
conditions:
  - condition: or
    conditions:
      - condition: template
        value_template: "{{ trigger.platform == 'state' }}"
      - condition: state
        entity_id: sensor.printer_status
        state: "printing"
actions:
  - variables:
      status: "{{ states('sensor.printer_status') }}"
      pct: "{{ ((states('sensor.printer_progress') | float(0) / 5) | round(0) * 5) | int }}"
      left: "{{ ((states('sensor.printer_remaining_time') | int(0) + 4) // 5) * 5 }}"
      name: "{{ states('sensor.printer_task_name')[:20] }}"
  - action: rest_command.esl_tag_text
    data:
      mac: "00000123456789A8"
      fields: >-
        {%- if status == 'printing' -%}
        {{ ['Printing: ' ~ pct ~ ' %', name,
            'Left: ' ~ (left // 60) ~ ' h ' ~ (left % 60) ~ ' min',
            'Done at ' ~ (now() + timedelta(minutes=left)).strftime('%H:%M')] }}
        {%- else -%}
        {{ ['Printer: ' ~ status, name, 'Updated ' ~ now().strftime('%H:%M')] }}
        {%- endif -%}
```

Change `printing` to the state your integration uses (for example `running` for Bambu Lab). While printing the tag shows, for example, `Printing: 40 %`, `Benchy.3mf`, `Left: 1 h 25 min` and `Done at 15:54`.

## With a progress bar

![The prusa layout on a 2.9" tag: PRUSA in a black band, 40 % in large digits, time left, finish time and a progress bar](images/ha-3d-printer.png)

The layout `prusa` in [tag/designs/layouts/3d-printer-2.9.json](../tag/designs/layouts/3d-printer-2.9.json) (2.9" only) has five fields: `name`, `pct`, `left`, `done` and `bar`. Layout files have no bar element, so the bar is an inverted text field of spaces: the black box hugs the text, and 31 spaces fill the frame. `pct` uses the `mid` font, which has digits only; the `%` is fixed text. Change the fixed text `PRUSA` to the name of your printer.

Copy the layout into the AP's 2.9" file (`GET /api/layouts/2.9`, add it to `layouts`, `PUT` it back, or `esl-client layouts` and `layout-put 2.9 FILE`). The compiled file holds at most 2048 bytes and the layout takes 181, so drop example layouts no tag uses if it does not fit.

Then the automation sends `layout: prusa` and the five fields (entities from the PrusaLink integration). It leaves the file name out, and shows the status and material when the printer is idle. It runs every 10 minutes while printing and rounds to 10 % and 10 minutes, so the tag redraws at most 6 times an hour:

```yaml
alias: 3D printer to ESL tag
mode: queued
max: 2
triggers:
  # Only when the state itself changes (to: null ignores attribute changes)
  - trigger: state
    entity_id:
      - sensor.prusa_core_one
      - sensor.prusa_core_one_material
    to: null
  # Every 10 minutes while printing: at most 6 redraws an hour
  - trigger: time_pattern
    minutes: /10
conditions:
  - condition: or
    conditions:
      - condition: template
        value_template: "{{ trigger.platform == 'state' }}"
      - condition: state
        entity_id: sensor.prusa_core_one
        state: printing
actions:
  - variables:
      status: "{{ states('sensor.prusa_core_one') }}"
      # Rounded so the text only changes in steps: 10 %, 10 min
      pct: "{{ ((states('sensor.prusa_core_one_progress') | float(0) / 10) | round(0) * 10) | int }}"
      fin: "{{ ((as_timestamp(states('sensor.prusa_core_one_print_finish'), 0) / 600) | round(0) * 600) | int }}"
      left: >-
        {{ (((([fin - as_timestamp(now()), 0] | max) / 600) | round(0, 'ceil')) | int * 10) if fin else 0 }}
  - action: rest_command.esl_tag_text
    data:
      mac: "00000123456789A8"
      layout: prusa
      # name, pct, left, done, bar (31 spaces = full bar)
      fields: >-
        {%- if status in ['printing', 'paused'] -%}
        {{ ['', pct | string,
            'Left ' ~ (left // 60) ~ ' h ' ~ (left % 60) ~ ' min',
            'Done at ' ~ (fin | timestamp_custom('%H:%M') if fin else '?'),
            ' ' * ((pct | int * 31 / 100) | round(0) | int)] }}
        {%- else -%}
        {{ ['', '-', status | capitalize, 'Material: ' ~ states('sensor.prusa_core_one_material')] }}
        {%- endif -%}
```

## Limits and timing

- **Text:** 4 fields (lines), 92 characters in all after conversion to Latin-1 (96 bytes with the length bytes, [protokoll.md](protokoll.md) part 1). æøå work. Over the limit the AP answers `422` and the tag keeps what it shows.
- **Line width:** about 20 characters per line on the 2.9" before the text drops to a smaller font or is cut with "…". The 1.54" wraps to two lines per field.
- **When it shows:** the tag polls the AP every 60 s, so a change shows within about a minute plus drawing.
- **Battery:** the AP compares the content. The same text again gives `changed: false` and the tag does not draw again, which is why the numbers are rounded (5 % and 5 min): a new screen costs power (the image download and the refresh), the same text again does not. A 6-hour print gives about 70 updates, while the power budget ([strombudsjett.md](strombudsjett.md)) assumes about one a day, so for daily use 10 % or 10 min is kinder. Do not lower the tag's `poll_s` for this.
- **"Done at":** changes when the estimate moves, which is an extra redraw. Remove the line if that is too often.

## Test without the automation

Developer tools → Actions → `rest_command.esl_tag_text`:

```yaml
mac: "00000123456789A8"
fields: ["Printing: 42 %", "Benchy.3mf", "Left: 1 h 25 min", "Done at 17:40"]
```
