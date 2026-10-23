# Choosing an access point board

The AP firmware runs on several ESP32 boards. All of them do the same job: the HTTP API, the dashboard, storing what each label shows, drawing the screens, and the 802.15.4 radio to the labels.

| Board | Labels | Price (about) | Status |
|---|---|---|---|
| [ESP32-C6](ESP32-C6) (Super Mini) | about 700 | 5 $ | supported, the one tested most |
| [ESP32-C6 with screen](ESP32-C6-with-screen) | about 700 | 10 $ | supported |
| [ESP32-C5](ESP32-C5) N16R8 | 1000 | 10–15 $ | experimental |
| [ESP32-S3 + C6](ESP32-S3-and-C6) (with or without screen on the C6) | 1000 | 10 $ + a C6 | experimental |
| [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) (no wire between them) | not decided | 10 $ + a C6 | planned, with real limits |

"Experimental" means the firmware builds and is in every release, but it has not been tried on that board yet. Reports are welcome.

## Which one?

- **Start with an ESP32-C6.** One small, cheap board does WiFi and the label radio, and it is flashed from the web page.
- **Take the one with a screen** if you want to see the AP's address, WiFi and label count without opening anything. Handy while setting up.
- **Go for the S3 + C6** when you need more than about 700 labels, or want the label radio never to wait for WiFi.
- **The S3 with a wireless C6** is for when the radio cannot sit next to the AP. Changes reach the labels 10–30 s later and some replies are missed; read its limits first.
- **The C5** is one chip with room for 1000 labels and WiFi on 5 GHz, but it is newer and less common.

For more labels than one AP takes, use several APs on different radio channels.

The full comparison with antennas: [docs/access-points.md](https://github.com/mathiasbk/openESL/blob/master/docs/access-points.md). Each board's files: [ap/boards](https://github.com/mathiasbk/openESL/blob/master/ap/boards/README.md).
