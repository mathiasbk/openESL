# wemos-debug

A D1 mini (ESP8266) that reads the tag's debug UART and forwards it to the PC.

Wiring:

| D1 mini | Tag (ZBS243) |
|---------|--------------|
| D5      | debug pad 4 (UART TX) |
| GND     | GND |

The board only listens; nothing drives the tag. Tag UART is 9600 baud, USB serial is 115200.

```sh
cd tools/wemos-debug
pio run -t upload        # only this D1 mini plugged in, or pass --upload-port
pio device monitor
```

Keep this board separate from the flasher D1 mini running ZBS_Flasher.
