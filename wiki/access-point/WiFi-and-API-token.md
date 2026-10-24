# WiFi and API token

A freshly flashed AP needs two things: a WiFi network to join, and an **API token** that every client must send.

## Over USB, right after flashing

On the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab (step 2) or the [Set up](https://mathiasbk.github.io/openESL/#setup) tab, press **Connect AP**. The page shows the networks the AP can see, fills in a random token if the AP has none, and watches it join. Close anything else that has the AP's port open first, such as `esl_client.py ... serve`.

## From a phone, on the setup network

With no WiFi saved, the AP opens its own network `openesl-XXXX`. Join it from a phone, and the "sign in to network" page opens the setup form (or go to `http://192.168.4.1/setup`). Pick your network, enter the password, and choose the API token.

If the saved network stops working (a new WiFi password, say), the setup network opens again after three minutes, so you can fix it without a cable.

## The API token

- Every API call needs `Authorization: Bearer <token>`.
- Five wrong tokens in a row lock the API for 60 seconds.
- The AP never shows the token again. **Lost it?** Set a new one over USB, which needs no old token:
  `python3 tools/esl-client/esl_client.py --serial /dev/ttyACM0 wifi --new-token NEWTOKEN`, or the Set up tab.
- The USB port needs no token: whoever has the cable has the device.

## Finding the AP on the network

The AP is `openesl-xxxx.local` (mDNS), with the same four digits as its setup network `openesl-XXXX`, so two APs on one network get their own names. An AP that already had its WiFi set up before 0.6.1 keeps `openesl.local`. Or look up its IP address in your router. The examples in this wiki use `openesl.local`: put your AP's name in. To change the name: `esl_client.py wifi --hostname NAME`.

Next: [Flashing labels](Flashing-labels), or [AP settings](Access-point-settings).
