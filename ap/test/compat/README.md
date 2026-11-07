# Compatibility with earlier releases

An AP is updated in place: it keeps what it stored in NVS, and its tags, some of them out of range for days, keep the firmware they have. A new release must therefore read what every earlier release stored, the same way, and answer older tags as before. These files check that on every pull request (issue #307).

## What is checked

| File | Written by | Checked by | What |
|------|------------|------------|------|
| `v<release>.txt` | `make_golden.py` (runs `gen_golden.c` against the AP core of that release) | `../test_compat.c` (`make` in `ap/test`) | What that AP left in NVS (layout file, content, tag settings, the "seen" record) and how it answered its tags after a restart from it |
| `tools/esl-client/compat/v<release>-backup.json` | `tools/esl-client/compat/make_backup.py` (that release's `esl_client.py backup` against the fake AP of its own tests) | `test_restore_backup_from_releases` in `tools/esl-client/test_esl_client.py` | That today's `restore` takes the file |

For each `v<release>.txt`, today's AP core must:

- decode every stored entry, and give each tag's content the same `content_id` as that release did (also in `GET /api/tags/{mac}`), so no tag is sent its content again after the update;
- read the same tag settings and "seen" record (a settings entry from before 0.9.0 has no long backoff, and gets the defaults);
- load the same layout file;
- answer the same POLL, from the tags in the scenario below, with the same frame. The one exception is a tag that shows the AP's images and is sent a new image, because its content now draws differently (fonts or layout code changed): the test prints a note for it and does not fail, since the tag knows the IMG message. Any other difference fails.

The tags in the scenario (`gen_golden.c`): a 2.9" on tag firmware 0.6.0 with built-in `price` content and its own poll interval, a 1.54" on 0.9.2 with `text` content and the long backoff settings, a 2.9" with the 14-byte POLL of firmware before 0.9.0 and content from a layout file (stored with the layout's name), a tag with the 12-byte POLL of firmware without CONFIG, and a 1.54" that has only polled.

## Adding a release

In the VERSION pull request of the next release, add the files of the release before it, which is tagged by then:

```
python3 ap/test/compat/make_golden.py v0.10.0
python3 tools/esl-client/compat/make_backup.py v0.10.0
```

Both take that release's code from git without touching the working tree. Commit the new files and never write them again for the same release: the point is that they hold what that release wrote, not what today's code would write. A file that fails after a change means the change breaks an update from that release; change the code, not the file.

Not covered yet (#307): firmware updates of the tags in the simulator from the previous release's tag firmware, and backup files from the dashboard.
