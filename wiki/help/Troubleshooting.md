# Troubleshooting

See also the [FAQ](FAQ).

## The access point


**The flasher does not find the board.** Hold **BOOT** while you plug it in, and use Chrome or Edge on a computer. Close anything else that has the port open (`esl_client.py serve`, a serial monitor).

**I lost the API token.** Set a new one over USB, which needs no old token: the [Set up](https://mathiasbk.github.io/openESL/#setup) tab, or `esl_client.py --serial /dev/ttyACM0 wifi --new-token NEWTOKEN`.

**All my requests get 429.** Five wrong tokens in a row lock the API for 60 seconds. Wait and use the right token.

**The AP is not on the network after I changed WiFi password.** After three minutes without a connection it opens its setup network `openesl-XXXX` again. Join it from a phone and pick the network again.

**`openesl.local` does not work.** An AP set up after 0.6.0 is `openesl-xxxx.local`, with the four digits of its setup network; the WiFi card on the web page and `esl_client.py wifi` over USB show the name. Some networks and Windows setups do not resolve `.local` names. Use the AP's IP address from your router, or from the screen on boards that have one.

**The web page cannot reach my AP over WiFi.** That is on purpose in browsers: a page on https may not call a plain http address on your network. Open the dashboard on the AP itself (`http://openesl.local/dashboard`), or connect over USB. To send a layout, use **WiFi → Open my AP** in the designer's Send dialog: it opens the AP's own page with the file ([Getting layouts onto the AP](Getting-layouts-onto-the-AP)). Developers can also run the bridge (`esl_client.py serve`).

**Open my AP opens the dashboard, but no layout file shows up.** The AP's firmware is older than this feature (before 0.9.0): update it from the Dashboard's Firmware card, or send over USB. Also check that the tab really is your AP (the address), and that the browser did not block the new tab.

**I flashed the AP and everything is gone.** It was flashed with **Erase and flash as new** on the Flash AP tab, or with the single `.bin` at 0x0. Restore from your backup (`esl_client.py restore ap.json`). See [Flashing the AP](Flashing-the-access-point#flashing-again-without-losing-anything).
**The S3 + C6 AP is on the network, but no label shows up.** The C6 radio does not hear the S3, or the S3 does not hear the C6. Read the C6's log on its USB port: [Checking the link](ESP32-S3-and-C6#checking-the-link) tells which wire is wrong from what it says. The usual faults are TX and RX not crossed, a loose GND, and the C6's TX in the S3's GPIO16 instead of GPIO18.

## The labels

**The label shows "No AP found".** It cannot hear the AP. Check that the AP runs OpenESL firmware and is in range. The label tries again less often to save battery, so give it a few minutes after the AP is back, and up to an hour if the AP was gone for more than a day.

**The dashboard says the signal is weak.** The AP hears the label below about −85 dBm on average, or some of its check-ins were lost, or a quarter of them needed a second try (the bars in the Signal column show which; see [Dashboard](Dashboard#signal)). The label still works, but it is near the edge of the range and may drop out. Move the AP or the label closer, put the AP higher with a clear view of the labels, or away from metal shelves and the WiFi router. A higher transmit power ([Access point settings](Access-point-settings)) only helps the AP's answers reach the label, not the label's check-ins reach the AP.

**The label does not show what I sent.** Look it up on the dashboard: it says what the label is waiting for or what is wrong. A label shows new content at its next poll, so allow a minute or so. A label on firmware 0.4.x cannot show screens from a 0.5.0 AP; update it ([Firmware updates](Updating-labels)).

**The battery shows 0.** The 1.54" cannot measure its battery voltage yet, so it reports 0 (unknown).

Still stuck? [Open an issue](https://github.com/mathiasbk/openESL/issues) with what you tried and what the dashboard says.
