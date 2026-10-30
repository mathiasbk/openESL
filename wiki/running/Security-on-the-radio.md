# Security on the radio

Out of the box, anyone in radio range with an 802.15.4 radio who speaks the OpenESL protocol can change what a label shows, or give it firmware. **Pairing** closes that: a paired label signs everything it sends, and takes content, settings and firmware only from an AP that has its key. Prices are still sent in the clear, so others can read them; they just cannot change them.

Needs AP firmware 0.9.0 and label firmware from the 0.9.0 release ([Updating labels](Updating-labels)). An ESP32-S3 with a wireless C6 cannot pair labels yet.

## Pairing labels

1. On the [dashboard](Dashboard), tick the labels and press **Pair**. Or: `python3 esl_client.py pair 00000123456789A8`.
2. At its next poll, each label gets its own key. A lock shows next to its MAC in the label list once it signs (`secure: true` in `GET /api/tags`).
3. When every label has the lock: **Settings → Radio → Only paired labels**, and Save. Or: `esl_client.py config --radio-secure on`. From then on the AP ignores everything that is not signed, so a stranger's label, or a label that is not paired, gets no answer.

The key goes to a label unsigned once, when it is paired. Pair where nobody is listening in, for example before the labels go up in the shop. A label that is already paired gets a new key only signed with its old one.

**Unpair** (dashboard) or `esl_client.py unpair MAC` makes a label forget its key at its next poll. Turn "Only paired labels" off first, or the AP stops answering it. A label that was shut out for a while and shows "No AP found" can take several minutes to find the AP again once "Only paired labels" is off (it searches less often while it has no AP).

## The key and the backup

The AP makes its radio key the first time it starts with 0.9.0. Every label's key is made from it, so the AP keeps no list. The key is in the [backup](Backup-and-restore), so **keep backup files secret**. A restore onto a new AP puts the key back, and the paired labels carry on. Without the key (a full erase and no backup), paired labels get no answers until they are flashed again in the [jig](Flashing-jig).

## What it does not protect

- Nothing is encrypted: anyone in range can read the prices.
- A label that is not paired is as open as before, and someone else in range can pair it with their own key; it then has to be flashed again in the jig. Pair your labels soon after they go up.
- Until "Only paired labels" is on, someone can make a paired label look unpaired in the dashboard for a while (they still cannot change what it shows).
- Someone with the AP in their hands, or the API token, can do anything, including reading the key.

More detail for developers: [docs/protokoll.md](https://github.com/mathiasbk/openESL/blob/master/docs/protokoll.md), "Security", and [SECURITY.md](https://github.com/mathiasbk/openESL/blob/master/SECURITY.md).
