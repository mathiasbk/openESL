# Glossary

The words this wiki uses, in plain terms.

| Word | What it means |
|---|---|
| **Label**, **tag**, **ESL** | The e-paper shelf label itself. ESL means electronic shelf label. The wiki says "label"; the web page, the API and the code often say "tag". They are the same thing. |
| **Access point (AP)** | A small ESP32 board on your WiFi that talks to the labels by radio. It keeps what each label should show, draws the screens, and has the HTTP API and the dashboard. See [Choosing a board](Access-point-boards). |
| **Model** | A kind of label, such as the [SoluM 2.9"](SoluM-2.9-ST-GM29XX2). Each model has its own screen size, so a layout has a version per model. See [Supported labels](Supported-labels). |
| **MAC** | The label's unique address, 16 hex digits such as `000001C287FC3B1A`. You use it to say which label to change. It is on the label's start screen and on the dashboard. |
| **Layout** | The design of a screen: where the text, price, lines and barcodes go. The AP fills it with your text. Made in the [Label designer](Label-designer). |
| **Template** | A ready-made layout to start from, such as "Shelf price" or "Weather". See [Label templates](Label-templates). |
| **Fields** | The pieces of text you send for a layout, for example the product name, the price and the unit price. |
| **Poll** | A label waking up and asking the AP "anything new?". By default every 60 seconds, so a change shows within about a minute. |
| **Dashboard** | The page on the AP that lists every label, what it shows, its battery and signal. See [Dashboard](Dashboard). |
| **API token** | The password your software sends with every request to the AP. You choose it when you set up the AP. See [WiFi and API token](WiFi-and-API-token). |
| **Jig** | A holder with spring-loaded pins that touch the test pads inside a label, so it can be flashed. You need it once per label. See [The flashing jig](Flashing-jig). |
| **Flashing** | Writing new firmware to a chip. The AP is flashed over USB, a label once in the jig. |
| **OTA** | "Over the air": updating firmware by radio or WiFi instead of a cable. Labels take new firmware this way after the first flash. See [Updating labels](Updating-labels). |
| **Firmware** | The program that runs on the AP or on a label. |
| **802.15.4** | The low-power radio standard the labels use, on 2.4 GHz like WiFi but separate from it. |
| **Channel** | Which of the 802.15.4 radio channels (11–26) the AP and its labels use. Two APs near each other should use different channels. |
| **esl-client** | A command-line tool for the AP: send content, back up, update. See [esl-client](esl-client). |
| **Simulator** | Runs the label firmware and the AP on a PC, to try things without hardware. |

Missing a word? [Tell us](https://github.com/mathiasbk/openESL/issues).
