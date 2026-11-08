# Why labels stay connected

openESL is built around one rule: you should not lose a label. A label that falls off the network, forgets its content or has to be flashed again costs more than any feature saves. This page shows what happens in the situations that usually go wrong, what keeps each label working, and the few limits you should know about.

## The short version

- **The label asks, the AP answers.** A label wakes up on its own (every 60 seconds by default) and asks the AP whether there is anything new. The AP never has to reach a sleeping label, so there is no "push" that can be missed.
- **Both sides keep their state through a restart.** The AP keeps the label table, each label's content and settings, the layouts and the scheduled content in its flash. The label keeps its picture and its settings in its own flash.
- **Nothing is confirmed until it has arrived.** A label reports the id of the picture it shows in every poll. Until that id matches, the AP keeps offering the new picture. Only a label that turns a picture down (the wrong size for it, or a broken download) makes the AP stop, and the label then shows as `draw_failed` on the [Dashboard](Dashboard).
- **Nothing is sent twice.** The content id is a checksum of what the label shows. Sending the same content again changes nothing, and the label does not redraw.

## What happens when …

| Situation | What the label does | What you need to do |
|---|---|---|
| The AP restarts or loses power | Keeps showing its content. Its next poll after the AP is back gets an answer, as before. The AP still has its table, content and settings; until each label has polled, the dashboard shows it as not seen yet. Scheduled content waits until the AP has the time again from the internet. | Nothing. |
| The AP is gone for a while | After 3 missed polls it polls less often, up to every 10 minutes. After 2 hours (`lost_ap_s`, set per label with `PUT /api/tags/{mac}/config` ([API reference](https://github.com/mathiasbk/openESL/blob/master/docs/openapi.yaml))) it draws its picture again with a small "no AP" mark in the corner, so you can see it on the shelf. The content stays up, and when the AP answers again the mark goes away without a new download. If a new picture was half way in when the AP went, the label has no whole picture to show, so it shows "No AP found" until the AP is back and sends the picture again ([#391](https://github.com/mathiasbk/openESL/issues/391) will keep the old picture up instead). | Nothing. Bring the AP back. |
| The AP is gone for a day or more | It polls once an hour instead of every 10 minutes, to save the battery, and finds the AP again within the hour once it is back on the same channel. | Nothing. |
| You change the radio channel | Every label is told the new channel first, and the AP moves only once the labels it has heard lately have confirmed it, or after a set time. A label that missed the move (it was switched off, say) looks on its last channel, on the announced one, on the built-in one, and then on one channel after the other. That can take a few hours. | Update every label to 0.9.2 or newer first (the AP refuses to move while it hears one older than 0.9.0). Then nothing. The [Dashboard](Dashboard) shows how many labels have confirmed. Read [Upgrading from older versions](Upgrading-from-older-versions) before you do it. |
| A label's battery is changed | It shows its last picture straight away, with its settings from flash, and polls as before. If it was in the middle of fetching a new picture, it shows its start screen until the AP sends that picture at its first poll. | Nothing. |
| A picture or update is cut off half way | The AP offers the rest at the next poll, and the label continues from the block where it stopped, as long as it has not restarted in between. After a restart it starts that download over. | Nothing. |
| You update the AP over the air | AP firmware goes into the second app slot. The running one is untouched until the new one is complete and checked, the AP goes back to the old one if the new one does not start properly, and the label table is kept. Labels with older firmware keep working. Flashing over USB with **Erase** wipes the table, so take a backup first. | Follow the order in [Updating the AP](Updating-the-access-point). |
| You update a label | The new firmware is written next to the running one and checked before the label switches. The label keeps its content and settings. | See [Updating labels](Updating-labels). |
| You move to a new AP | The backup holds the settings, layouts, every label's content and the scheduled content, and a restore puts them on the new AP. Data sources (weather, calendar and the rest), the WiFi password and the API token are not in it. | See [Backup and restore](Backup-and-restore) and [Moving to a new AP](Moving-to-a-new-AP). |

## More than one AP

From 0.11.0 every AP has its own id, and a label with label firmware 0.11.0 or newer remembers the id of its AP. While a label looks for its AP after a power cut or an update, a neighbouring openESL AP leaves it alone, so it finds its own AP again when that is back, on whatever channel ([#390](https://github.com/mathiasbk/openESL/issues/390)).

To move labels on purpose:

- **A new AP in place of a broken one:** restore the old AP's backup and choose *In place of the AP the backup came from*. The new AP gets the old id, and the labels take it at once ([Moving to a new AP](Moving-to-a-new-AP)).
- **Without a backup:** a new AP (or one whose memory was erased) takes over lost labels of any AP for its first 14 days. A label is taken once it has been without its own AP for an hour, so expect an hour without updates, and put the content on again.
- **From one AP to another:** on the AP that should get them, choose **Settings → Radio → Labels of other APs** and the other AP's id. Labels are taken once they have been without their own AP for an hour, for 14 days. The other AP's labels show up in [Health](Health-and-webhook) when they poll here.
- **The last way out:** a label that has heard nothing from its own AP for a week takes any AP.

Labels with older firmware (and APs before 0.11.0) work as before: every AP answers them, so on a site with several APs such a label can still end up with a neighbour. Update the labels to 0.11.0 to stop that. Taking a battery out does not help: the label keeps its AP's id, and the restart only starts its hour again.

## How you notice a problem early

- The [Dashboard](Dashboard) shows each label's last poll, signal and battery, and lists labels that have gone quiet.
- [Health and webhook](Health-and-webhook) flags a label that has missed 3 of its polls (`silent`), one not heard from since the AP started (`unseen`) and a low battery. On an S3 AP it also flags a C6 radio that has lost contact. A webhook or [Home Assistant](Home-Assistant) can tell you at once.
- With label firmware 0.9.0 or newer, each poll also says how the link has been: the signal both ways and how many rounds went unanswered, so a label on the edge of range shows up before it drops off.

## Batteries

With the default settings and one screen change a day, a label that hears its AP is calculated to use about 4.6 µA on average. That is roughly 9 years on one CR2450 for a 1.54" label, and more for a 2.9" label with two cells, so in practice the shelf life of the cell is the limit. A label that changes 24 times a day uses more, about 4 to 5 years on one cell. The numbers and the assumptions behind them are in [docs/power-budget.md](https://github.com/mathiasbk/openESL/blob/master/docs/power-budget.md). These are calculations, not measurements; measured values from real labels will be added here as they come in.

## Where this is described in detail

- The protocol, including the channel change and how a label finds a lost AP: [docs/protocol.md](https://github.com/mathiasbk/openESL/blob/master/docs/protocol.md)
- How labels are updated over the air: [docs/tag-ota.md](https://github.com/mathiasbk/openESL/blob/master/docs/tag-ota.md)
- [How it works](How-it-works) for the bigger picture
