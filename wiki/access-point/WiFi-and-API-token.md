# WiFi and API token

A freshly flashed AP needs two things: a WiFi network to join, and an **API token** that every client must send.

## Over USB, right after flashing

Right after flashing, step 3 of the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab does this over the same port. Later, on the [Set up](https://mathiasbk.github.io/openESL/#setup) tab, press **Connect AP**. On an S3 or C5 board with two USB ports, use the one marked **USB**: the other (COM) can flash, but the WiFi setup gets no answer there. The page shows the networks the AP can see, fills in a random token if the AP has none, and watches it join. Close anything else that has the AP's port open first, such as `esl_client.py ... serve`.

## From a phone, on the setup network

With no WiFi saved, the AP opens its own network `openesl-XXXX`. Join it from a phone, and the "sign in to network" page opens the setup form (or go to `http://192.168.4.1/setup`). Pick your network, enter the password, and choose the API token.

If the saved network stops working (a new WiFi password, say), the setup network opens again after three minutes, so you can fix it without a cable. The AP keeps retrying the saved network meanwhile, and closes the setup network a minute after it has joined.

The WiFi and the token are kept across restarts and firmware updates. Erasing the flash clears them.

To move the AP to another WiFi network, do it over USB or on the setup form: changed over WiFi (`PUT /api/wifi`), the AP drops the old network at once, and with it your connection.

## The API token

- Every API call needs `Authorization: Bearer <token>`.
- A token is 8 to 128 characters: letters, digits and punctuation, no spaces.
- There is one token for the whole AP, with no users or roles: treat it like a password, and keep the AP on your local network or VPN (the API is plain HTTP).
- Five wrong tokens in a row lock the API for 60 seconds, also wrong tokens typed in the setup form. The USB port is never locked.
- The AP never shows the token again. **Lost it?** Set a new one over USB, which needs no old token:
  `python3 tools/esl-client/esl_client.py --serial /dev/ttyACM0 wifi --new-token NEWTOKEN`, or the Set up tab.
- **Changing it** over WiFi: open the setup form (linked from the status page) and enter the current token and a new one, or send `PUT /api/wifi` with `{"token": "..."}` and the current token. Once a token is set, the form asks for it before changing anything.
- The USB port needs no token: whoever has the cable has the device. Neither does the read-only status page at `http://openesl-xxxx.local/`.

## Finding the AP on the network

The AP is `openesl-xxxx.local` (mDNS), with the same four digits as its setup network `openesl-XXXX`, so two APs on one network get their own names. An AP that already had its WiFi set up with 0.6.0 or older keeps `openesl.local`. Two of those on one network share the name until you rename one: `esl_client.py --serial /dev/ttyACM0 wifi --hostname openesl-s3`. Or look up its IP address in your router. The examples in this wiki use `openesl.local`: put your AP's name in. To change the name: `esl_client.py wifi --hostname NAME`.

Next: [Flashing labels](Flashing-labels), or [AP settings](Access-point-settings).
