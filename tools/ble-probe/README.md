# ble-probe: learning a Bluetooth tag's protocol

Bluetooth (BLE) price tags run their maker's firmware, and the maker's phone app sends
them their pictures. Before openESL can drive such a tag, we need to know what the app
sends: which characteristic, in what order, in what pieces. These two tools find that out
from our own recordings of our own tags.

| Tool | What it does | Needs |
|---|---|---|
| `scan.py` | lists the BLE devices nearby (name, address, signal, maker data, services), and with `--connect` reads a tag's GATT table | `pip install bleak` and a Bluetooth adapter |
| `btsnoop.py` | reads a phone's Bluetooth HCI log and prints every write, notification and read per connection, with the characteristic UUIDs | Python only |

Both only read: neither writes anything to a tag.

## Clean-room

openESL's protocol modules are written only from recordings we make ourselves and from
a maker's published documentation. Do not read other projects' code or notes for a tag's
protocol before writing ours, and write what we learn in our own words under `docs/`.

## 1. Find the tag

```sh
python3 tools/ble-probe/scan.py --seconds 15
```

Run it with the tag next to the PC; the strongest signals are first. Note its address
and name, and the maker data: a tag often puts its model or battery level there.

```sh
python3 tools/ble-probe/scan.py --connect AA:BB:CC:DD:EE:FF --json tag-gatt.json
```

lists its services and characteristics (`write`, `write-without-response`, `notify`) and
reads the ones that can be read. A tag that is asleep may not answer: press its button,
or connect right after it has advertised.

## 2. Record the maker's app (Android)

1. Settings, About phone: tap Build number seven times to turn on the developer options.
2. Developer options: turn on **Enable Bluetooth HCI snoop log** (on some phones: "Full"),
   then turn Bluetooth off and on.
3. In the maker's app, send the tag one simple, known picture: all white, then all black,
   then one with a black box in a corner. One picture per recording keeps it readable;
   note the time of each.
4. Get the log: `adb bugreport bugreport.zip`, and in the zip
   `FS/data/misc/bluetooth/logs/btsnoop_hci.log` (the path varies by phone; search the zip
   for `btsnoop`). Some phones also let you copy it from the file manager.
5. Turn the snoop log off again.

## 3. Read the recording

```sh
python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --summary
python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --addr AA:BB:CC:DD:EE:FF
python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --addr AA:BB:CC:DD:EE:FF --handle 0x21 --full
python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --json > session.json
```

`--summary` shows, per connection, the characteristics the app found and how many writes
and how many bytes went to each: the one that got about a screen's worth of bytes is the
picture. The default listing shows each write (`->`, to the tag) and notification (`<-`,
from the tag) with its time, so the order of commands, acknowledgements and picture
blocks can be read off. `--all` adds service discovery and responses.

What to work out, and write down in `docs/` for the tag:

- the command that starts a picture, and what it carries (size, colour planes, a picture id?)
- how the picture is split into blocks and what each block's header is
- what the tag answers after each block, and how the app finds a lost block
- the command that ends the picture and starts the screen update
- how the pixels are packed: compare the all-white, all-black and box pictures
- anything that looks like a key or a challenge before the first write (that tag then
  needs more thought before we support it)

## 4. Without a phone: a sniffer

A Nordic nRF52840 USB dongle (PCA10059) with Nordic's nRF Sniffer for Bluetooth LE
firmware shows the traffic between any app and the tag in Wireshark. Save the capture in
btsnoop format only if it was taken on the HCI side; air captures are read in Wireshark
directly. The phone's HCI log is usually the easier start.
