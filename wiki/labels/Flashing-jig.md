# The flashing jig

The jig holds one label at a time with pogo pins on its debug pads, so you do not have to hold the pins. It is a 5 × 7 cm perfboard with a Wemos D1 mini running atc1441's [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher), and three 3D-printed parts.

## Parts

- Wemos D1 mini (ESP8266)
- 5 × 7 cm perfboard
- Pin headers for the D1 mini (2 × 8)
- Pogo pins, one per hole in the upper part
- Wire, and a USB cable for the D1 mini
- The three printed parts (bottom, upper, insert)

## Building it

1. Build the flasher: a D1 mini with ZBS_Flasher, a small power switch and the wires to the pogo pins. [Building the flasher](Building-the-flasher) has the parts, the wiring, the ZBS_Flasher settings and how to test it.
2. Print the three parts and put the pogo pins in the upper part.

Photos, the printable files and the details: [Jig/Hardware](https://github.com/mathiasbk/openESL/blob/master/Jig/Hardware/README.md).

## Other flashers

ZBS_Flasher also runs on an ESP32 or an Arduino Nano, and on most boards with about eight free pins and a serial port. OpenESL is tested with the D1 mini. If you use something else, [tell us](https://github.com/mathiasbk/openESL/issues).

Next: [Flashing labels](Flashing-labels).
