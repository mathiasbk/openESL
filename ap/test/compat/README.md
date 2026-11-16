# Compatibility with earlier releases

An AP is updated in place: it keeps what it stored in NVS, and its tags, some of them out of range for days, keep the firmware they have. A new release must therefore read what every earlier release stored, the same way, and answer older tags as before. These files check that on every pull request (issue #307).

## What is checked

| File | Written by | Checked by | What |
|------|------------|------------|------|
| `v<release>.txt` | `make_golden.py` (runs `gen_golden.c` against the AP core of that release) | `../test_compat.c` (`make` in `ap/test`) | What that AP left in NVS (layout file, content, tag settings, the "seen" record) and how it answered its tags after a restart from it |
| `tools/esl-client/compat/v<release>-backup.json` | `tools/esl-client/compat/make_backup.py` (that release's `esl_client.py backup` against the fake AP of its own tests) | `test_restore_backup_from_releases` in `tools/esl-client/test_esl_client.py`, and `web/test/backup_compat.test.mjs` | That today's `restore` takes the file, and so does today's dashboard (the same format) |
| `web/test/compat/v<release>-dashboard-backup.json` (from 0.9.0) | `web/test/compat/make_backup.mjs` (that release's dashboard backup, `web/js/backup.js`, against today's simulated AP in `web/test/fake_backup_ap.mjs`) | `web/test/backup_compat.test.mjs`, and `test_restore_dashboard_backups_from_releases` in `tools/esl-client/test_esl_client.py` | That today's dashboard puts everything in the file back on a new AP (settings, font and layout files, content, the picture, a tag's own settings, scheduled content), and so does `esl_client.py restore` |
| (none: built from the release tag) | `sim/upgrade.py` (`make -C sim upgrade`) | `esl-sim upgrade` (`sim/upgrade.c`) | A tag on that release's firmware gets today's over the air from today's AP, and today's goes on from what the release's bootloader left: same screen, its settings, the update confirmed, not sent its image again. Then the same back to the release ([sim/README.md](../../../sim/README.md#upgrade-from-a-release-make-upgrade)) |

For each `v<release>.txt`, today's AP core must:

- decode every stored entry, and give each tag's content the same `content_id` as that release did (also in `GET /api/tags/{mac}`), so no tag is sent its content again after the update;
- read the same tag settings and "seen" record (a settings entry from before 0.9.0 has no long backoff, and gets the defaults);
- load the same layout file;
- answer the same POLL, from the tags in the scenario below, with the same frame. Two exceptions: a CONFIG may carry more settings after the ones that release sent (tags read the fields they know and skip the rest, `shared/proto.h`), and a tag that shows the AP's images may be sent a new image because its content now draws differently (fonts or layout code changed). The second is printed as a note ("is sent a new image") and does not fail, since the tag knows the IMG message. Any other difference fails.

The tags in the scenario (`gen_golden.c`):

| Tag | POLL | Content and settings | After the AP restarts |
|-----|------|----------------------|-----------------------|
| 2.9", firmware 0.6.0 | 14 bytes (before 0.9.0) | built-in `price`, own poll interval; the interval changes while it is away | CONFIG |
| 1.54", firmware 0.9.2 | 18 bytes | `text` with a frame, long backoff settings; the text changes while it is away | IMG |
| 2.9", firmware 0.6.0 | 14 bytes | `shelf` from the layout file (stored with the layout's name) | IDLE |
| 2.9", firmware without CONFIG | 12 bytes | `text`, own interval, which changes while it is away | IDLE with the new interval |
| 1.54", firmware 0.9.2 | 18 bytes | none, it has only polled | IDLE |

"Away" means the tag does not poll between the change and the AP's restart, as a tag out of range during an update. The radio channel stored in NVS is not covered yet.

## Adding a release

In the VERSION pull request of the next release, add the files of the release before it, which is tagged by then:

```
python3 ap/test/compat/make_golden.py v0.10.0
python3 tools/esl-client/compat/make_backup.py v0.10.0
node web/test/compat/make_backup.mjs v0.10.0
```

All three take that release's code from git without touching the working tree. The new `v<release>.txt` also adds the release to the simulated tag firmware updates (`sim/upgrade.py` tests every release that has one), and `backup_compat.test.mjs` fails until the release has its dashboard file. Commit the new files and never write them again for the same release: the point is that they hold what that release wrote, not what today's code would write. A file that fails after a change means the change breaks an update from that release; change the code, not the file. A "would be sent a new image" note means every tag with such content redraws once after the update, which costs battery: say so in the release's upgrade notes.
