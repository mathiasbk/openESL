# FAQ

Problems and fixes are on [Troubleshooting](Troubleshooting).

## General

**Does it need the internet or a cloud service?** No. The AP and labels work on your local network. The AP uses the internet only for the time (NTP), which you can also set from esl-client.

**How many labels can one AP serve?** About 500 on an ESP32-C6 (about 700 on the one with a screen), 1000 on the C5 or S3 + C6 ([Choosing a board](Access-point-boards)). For more, use several APs on different radio channels.

**Tag or label?** The same thing. See the [Glossary](Glossary).

**Can I try it without hardware?** Yes: the [simulator](https://github.com/mathiasbk/openESL/blob/master/sim/README.md) runs the AP and a label on a PC, and the [label designer](https://mathiasbk.github.io/openESL/#design) works in any browser.

**Do I need to know programming?** No. Flashing, designing layouts and sending content can all be done from the web page and the AP's dashboard, and [Home Assistant](Home-Assistant) has an integration. The HTTP API is there when you want to connect your own software.

**Can I use labels from other makers?** Not yet, but the project is built to grow there. See [Adding hardware](Adding-hardware).


## Labels

**How long does the battery last?** It depends on the poll interval and how often the screen changes. The estimate and how it is worked out are in [docs/strombudsjett.md](https://github.com/mathiasbk/openESL/blob/master/docs/strombudsjett.md).

**Can I get the original firmware back?** Yes, if you kept the backup the Flash tag tab made ([Label backups](Label-backups)). Write it back with ZBS_Flasher.

Still stuck? [Open an issue](https://github.com/mathiasbk/openESL/issues) with what you tried and what the dashboard says.
