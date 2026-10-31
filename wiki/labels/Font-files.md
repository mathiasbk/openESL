# Font files

The AP draws text with six built-in fonts: `small` (17 px), `text20` (20 px), `medium` (24 px), and for prices `mid` (36 px), `price44` (44 px) and `large` (52 px), which only have digits and ` ,-.:`. When you need another size, or a price font with letters, you can add a **font file** to the AP. No new firmware is needed, and nothing changes on the labels: the AP draws the image and the label only shows it.

Needs an AP and a web page from the release that has font files (`GET /api/fonts` answers on the AP).

## Make a font file

On a computer with Python, from the `tag/` folder of the repository:

```
pip install freetype-py pillow
python tools/gen_font.py --file text28 fonts/src/Inter-SemiBold.ttf 22 28 23
```

The numbers are the FreeType size, the height of the font in pixels and the rows above the baseline. It writes `text28.json` and `text28.png`, a picture of every character: look at it before you use the font. Options:

- `--chars latin1` (the default) gives all of Latin-1, `--chars price` only digits and ` ,-.:`, or give the characters themselves: `--chars "0123456789,-kr "`. Fewer characters make a smaller file.
- `--source "..."` says where the glyphs come from (kept in the file). Use a font whose licence lets you do this; Inter (in `tag/fonts/src`) is under the SIL Open Font License.

The name (`text28`) is what layouts call the font: 1 to 15 of `a-z`, `0-9`, `_` and `-`, and not the name of a built-in font.

## Put it on the AP

In the dashboard, open **Layouts** and press **Add a font file…** under *Font files*. Or with the API:

```
curl -X PUT http://openesl.local/api/fonts/text28 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  --data-binary @text28.json
```

The AP holds up to 8 font files of 32 KB together, kept across restarts and updates.

## Use it in a layout

In the [Label designer](Label-designer), connect to your AP: its font files are then in the font lists next to the built-in fonts, and the preview draws with them. In a layout file, name the font in `fonts` like a built-in one: `"fonts": ["text28", "medium"]`.

## Change or delete it

Send a font file with the same name again to replace it. The labels whose layouts use it are drawn again with the new one at their next poll. If the new font no longer fits a box it is used in (it is taller), the AP turns it down and keeps the old one.

A font file that a layout uses cannot be deleted: change those layouts first, then delete it under *Font files* in the dashboard (or `DELETE /api/fonts/text28`).
