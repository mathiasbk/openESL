# Why labels stay connected

openESL is built around one rule: you should never lose a label. A label that falls off the network, forgets its content or has to be paired again costs more than any feature saves. This page shows what happens in the situations that usually go wrong, and what keeps each label working.

## The short version

- **The label asks, the AP answers.** A label wakes up on its own (every 60 seconds by default) and asks the AP whether there is anything new. The AP never has to reach a sleeping label, so there is no "push" that can be missed.
- **Both sides keep their state through a restart.** The AP keeps the label table, each label's content and settings, the layouts and the scheduled content in its flash (NVS). The label keeps its image and its settings in its own flash.
- **Nothing is confirmed until it has arrived.** A label reports the id of the image it shows in every poll. Until that id matches, the AP keeps offering the new image, and an interrupted transfer goes on from where it stopped.
- **Nothing is sent twice.** The content id is a checksum of what the label shows. Sending the same content again changes nothing, and the label does not redraw.

## What happens when …

| Situation | What the label does | What you need to do |
|---|---|---|
| The AP restarts or loses power | Keeps showing its content. Its next poll after the AP is back gets an answer, as before. The AP still has its table, content and settings. | Nothing. |
| The AP is gone for a while | After `lost_ap_s` (2 hours by default) it draws its image again with a small "no AP" mark in the corner, so you can see it on the shelf. The content stays up. When the AP answers again, the mark goes away without a new download. | Nothing. Bring the AP back. |
| The AP is gone for a day or more | It polls once an hour instead of every minute, to save the battery, and finds the AP again within the hour once it is back. | Nothing. |
| You change the radio channel | Every label is told the new channel first, and the AP moves only once the labels it has heard lately have confirmed it, or after a set time. A label that missed the move looks on its last channel, on the built-in one and then on every channel until it finds the AP. | Nothing. The [Dashboard](Dashboard) shows how many labels have confirmed. |
| A label's battery is changed | It shows its last image straight away, with its settings from flash, and polls as before. | Nothing. |
| A picture or update is cut off half way | The AP offers the rest at the next poll, and the label continues from the block where it stopped. | Nothing. |
| You update the AP | AP firmware goes into the second app slot. The running one is untouched until the new one is complete and checked, and the label table is kept. Labels with older firmware keep working. | Follow the order in [Updating the AP](Updating-the-access-point). |
| You update a label | The new firmware is written next to the running one and checked before the label switches. The label keeps its content and settings. | See [Updating labels](Updating-labels). |
| You move to a new AP | The backup holds the settings, layouts, every label's content and the scheduled content, and a restore puts them on the new AP. | See [Backup and restore](Backup-and-restore) and [Moving to a new AP](Moving-to-a-new-AP). |

## How you notice a problem early

- The [Dashboard](Dashboard) shows each label's last poll, signal and battery, and lists labels that have gone quiet.
- [Health and webhook](Health-and-webhook) flags a label that has missed 3 of its polls (`silent`), one not heard from since the AP started (`unseen`), a low battery and a radio that has lost contact. A webhook or [Home Assistant](Home-Assistant) can tell you at once.
- Each poll also says how the link has been: the signal both ways and how many rounds went unanswered, so a label on the edge of range shows up before it drops off.

## Batteries

A label that hears its AP uses about 4.6 µA on average with the default settings. That is roughly 9 years on one CR2450 for a 1.54" label, and more for a 2.9" label with two cells, so in practice the shelf life of the cell is the limit. The numbers and the assumptions behind them are in [docs/power-budget.md](https://github.com/mathiasbk/openESL/blob/master/docs/power-budget.md). Measured values from real labels will be added here as they come in.

## Where this is described in detail

- The protocol, including the channel change and how a label finds a lost AP: [docs/protocol.md](https://github.com/mathiasbk/openESL/blob/master/docs/protocol.md)
- Label firmware updates and the rollback slot: [docs/tag-ota.md](https://github.com/mathiasbk/openESL/blob/master/docs/tag-ota.md)
- [How it works](How-it-works) for the bigger picture
