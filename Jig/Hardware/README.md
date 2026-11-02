# Jig hardware

The jig holds one tag at a time with pogo pins on its debug pads. It is made for a 5x7 cm perfboard with a Wemos D1 mini on headers. The D1 mini runs atc1441's [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher), and the PC talks to it over USB with [flash-tui](../../tools/flash-tui/README.md) or the Flash tag tab on the [web page](https://mathiasbk.github.io/openESL/).

## ZBS_Flasher

The D1 mini needs ZBS_Flasher built with OpenESL's pin settings, and a transistor switch for the label's power. The wiki page [Building the flasher](https://github.com/mathiasbk/openESL/wiki/Building-the-flasher) has the wiring, the settings and how to test it.

## BOM

- Wemos D1 mini (ESP8266)
- 5x7 cm perfboard
- Pin headers for the D1 mini (2 x 8)
- 2N3904 and BC557B transistors, 2 x 1 kΩ and 1 x 10 kΩ resistors (the power switch)
- Pogo pins, one per hole in the upper part
- Wire
- USB-C cable for the D1 mini
- The three printed parts below

## Photos

<img src="images/photo-board.jpg" width="24%"> <img src="images/photo-closed.jpg" width="24%"> <img src="images/photo-tag.jpg" width="24%"> <img src="images/photo-tag-angle.jpg" width="24%">

<img src="images/photo-29-tag.jpg" width="24%"> <img src="images/photo-pogo-wires.jpg" width="24%"> <img src="images/photo-pogo-pins.jpg" width="42.7%">

## Printed parts

The files are in [3D printet jig](3D%20printet%20jig/).

### Bottom

`jigg bottom.stl`

![Bottom](images/bottom.png)

### Upper

`Jigg upper.obj`

![Upper](images/upper.png)

### Insert

`jigg insert.obj`

![Insert](images/insert.png)
