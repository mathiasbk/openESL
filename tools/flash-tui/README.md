# flash-tui

A terminal UI for flashing many ZBS243 tags in a row: one tag at a time in the pogo-pin jig,
a D1 mini with atc1441's [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) firmware on USB.

For each tag it reads the MAC, saves a backup of the stock flash (first time only), writes the
OpenESL firmware and verifies it by reading it back. Every tag goes into a table on screen and a
line in `backup/flash_log.csv` (the `backup/` folder at the repo root, which git ignores; `--backup-dir` changes it).

Needs Python 3 with `textual`, `pyserial` and `rich`. Works in a normal terminal.

```sh
python tools/flash-tui/flash_tui.py            # with the flasher plugged in
python tools/flash-tui/flash_tui.py --fake     # simulated flasher and tag; settings and backups in a temp folder
```

## Keys

| Key | Does |
|---|---|
| space / Enter | flash the tag in the jig |
| m | switch model: 2.9" (296x128) / 1.54" (200x200) |
| f | firmware file for the current model |
| p | choose the flasher's serial port |
| a | auto-detect on/off: flash as soon as a tag answers in the jig |
| q | quit |

With auto-detect on, the tool polls the jig once a second. After a tag (flashed or failed) it waits
until the jig is empty before it takes the next one, so the same tag is not flashed twice.

## Firmware

Default paths are what `tag/tools/build.py` writes:

| Model | Build | File |
|---|---|---|
| 2.9" | `python tag/tools/build.py fw` | `tag/build/fw/openesl.bin` |
| 1.54" | `MODEL=154 python tag/tools/build.py fw` | `tag/build/fw154/openesl.bin` |

A missing file is shown in red and nothing is sent to the flasher. The image must end below 0xF400, where the tag keeps its layout file and last content (`CODE_SIZE` in `tag/tools/build.py`).

## Serial port

The debug Wemos ([tools/wemos-debug](../wemos-debug/)) is also a CH340, so the port is never guessed.

- The picker (`p`) lists every port with its USB path (e.g. `1-2.1:1.0`) and VID:PID.
- The choice is remembered by USB path in `~/.config/openesl/flash-tui.json` (also the model and
  firmware paths; `--config FILE` uses another file), because `/dev/ttyUSB0`/`1` can swap. The same USB socket gives the same path.
- Before anything else, the port must answer a ZBS_Flasher version request. A port that does not
  (the debug Wemos, anything else) is rejected and the picker opens again. The version request does
  not touch the tag.

## What happens per tag

1. The flasher powers the tag and checks the debug connection. No answer: "no contact with the tag".
2. The info page (1 KB) is read twice; the reads must match (a bad pogo contact gives noise).
3. **MAC.** If info page 0x10–0x17 holds a MAC (least significant byte first), that is used. Its
   model code (byte 6: 0x3B = 2.9", 0x34 = 1.54") must match the chosen model, else the tag is
   refused. If those bytes are blank (0xFF), the tag still runs the stock firmware: the MAC is
   built from flash 0xFC06–0xFC0B the same way as ZBS_Flasher's `mac` command
   (`00 00` + the 6 bytes, byte 6 = model code, byte 7 = 0x10 with a XOR checksum in the low nibble).
4. **Backup.** If there is no `backup/<MAC>.bin` yet (an older `backup/<MAC>/flash.bin` from
   `flash_loop.py` also counts), the whole 64 KB main flash is read twice, compared, and saved as
   `backup/<MAC>.bin`, with the info page as `backup/<MAC>.info.bin`. If anything goes wrong here,
   the tag is not written to. An existing backup is never overwritten.
5. **MAC to the info page** (stock tags only). The firmware reads its MAC from info page 0x10–0x17
   (`hal_mac_read`), but on a stock tag those bytes are blank, and erasing the main flash also
   erases the stock MAC at 0xFC06. So the 8 MAC bytes are programmed into the blank info page
   bytes, without erasing anything, and read back. This is done before the main flash is erased.
   This is what ZBS_Flasher's own "save MAC from firmware" command does too. `--no-mac-write`
   turns it off.
6. **Firmware.** The main flash is erased, the firmware written, and read back and compared.
7. The tag is power-cycled so it starts the new firmware.

The info page is never erased: the protocol module does not even have the command for it.

## Files

| File | Contents |
|---|---|
| `flash_tui.py` | the Textual app |
| `workflow.py` | the per-tag job (MAC, backup, write, verify) and the CSV log, no UI |
| `zbs_proto.py` | the ZBS_Flasher serial protocol, port listing, port probe |
| `settings.py` | the settings file |
| `fake_flasher.py` | simulated flasher + tag (in-memory flash and info page) for tests and `--fake` |
| `tests/` | unittest tests against the simulation |

`backup/flash_log.csv` columns: time, mac, model, mac_source, backup (saved/exists/failed),
firmware, fw_sha256 (first 16 hex digits), result, message, seconds.

## Tests

```sh
cd tools/flash-tui
python -m unittest discover -s tests -v
```

No test opens a real serial port (`tests/helpers.py` makes that fail) or touches real hardware.
The TUI tests drive the app headless with Textual's pilot.

## The ZBS_Flasher protocol

115200 8N1. Both directions use the frame `'A' 'T' cmd len data[len] crc_hi crc_lo`, where
`crc = (0xAB34 + cmd + len + sum(data)) & 0xFFFF`. The flasher answers with the same `cmd`.

| Cmd | Name | Data | Answer |
|---|---|---|---|
| 1 | GET_VERSION | – | 4 bytes, big endian |
| 10 | ZBS_BEGIN | slow SPI flag | 1 if the tag answers (RAM write/read test) |
| 11 | RESET_ZBS | – | power cycle, tag runs its firmware |
| 12 | SELECT_PAGE | 0 main flash, 1 info page | 1 if the page register reads back right |
| 22 | READ_FLASH | len (≤ 255), addr hi, addr lo | len bytes |
| 23 | WRITE_FLASH | len (< 252), addr hi, addr lo, data | 1 ok, 0 failed, 0xEE too long |
| 26 | ERASE_FLASH | – | erases the 64 KB main flash |

Notes from the firmware source: WRITE_FLASH skips bytes that are 0xFF (the page must be erased
first) and reads every byte back, retrying up to four times. When a byte fails it answers 0 *and
then* 1, so the client flushes its input after a failed write. Frames with a bad CRC get no answer.

## Credits and licence

The protocol is atc1441's (Aaron Christophel, [ATCnetz.de](https://ATCnetz.de)), from
[ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher); the reference PC client `zbs_flasher.py` is
by t.eddy, based on atc1441's original. `zbs_proto.py` is a new implementation written from reading
that firmware and client; no code is copied. The ZBS_Flasher repository has no licence file, which
is why this tool does not call or include `zbs_flasher.py`.
