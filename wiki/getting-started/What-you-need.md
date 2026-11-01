# What you need

Three things: labels, an access point (AP), and a jig to flash each label once.

| Part | What to get | More |
|---|---|---|
| **Labels** | A supported SoluM label: the [2.9" ST-GM29XX2](SoluM-2.9-ST-GM29XX2) (two CR2450) or the [1.54" ST-GM16XX2](SoluM-1.54-ST-GM16XX2) (one CR2450). Check the product name on the back. | [Supported labels](Supported-labels) |
| **Access point** | An ESP32-C6 board with USB is enough. Other boards take more labels. | [Choosing a board](Access-point-boards) |
| **For flashing labels** | A small board running atc1441's [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher), and a pogo-pin jig for the test pads behind the battery. Tested with a Wemos D1 mini; ESP32 and Arduino Nano also work. | [The flashing jig](Flashing-jig) |
| **A browser** | Chrome, Edge, or Firefox 151 or newer on desktop (Web Serial) for flashing. Firefox shows a site permission add-on prompt the first time. The designer works in any modern browser. | |

You only need the jig once per label. After that, labels get new firmware over the air ([Updating labels](Updating-labels)).

**No hardware yet?** The [simulator](https://github.com/mathiasbk/openESL/blob/master/sim/README.md) runs the label firmware and the AP together on a PC, so you can try the API first.
