# Font files

The AP draws text with six built-in fonts: `small` (17 px), `text20` (20 px), `medium` (24 px), and for prices `mid` (36 px), `price44` (44 px) and `large` (52 px), which only have digits and ` ,-.:`. When you need another size, or a price font with letters, you can add a **font file** to the AP. No new firmware is needed, and nothing changes on the labels: the AP draws the image and the label only shows it.

Needs an AP and a web page from the release that has font files (`GET /api/fonts` answers on the AP). Upgrade the AP before a layout uses a font file: an older AP firmware skips a layout file that names one (its labels keep what they show until it is back).

## Make a font file

In the dashboard, open **Layouts** and press **Make a font…** under *Font files*:

1. Give it a **name** (`text28`): that is what layouts call the font, 1 to 15 of `a-z`, `0-9`, `_` and `-`, and not the name of a built-in font.
2. Pick the **font**: one of the browser's (sans serif, serif, monospace), another font on your computer by its name (such as `Arial`), or a font file (`.ttf`, `.otf` or `.woff`).
3. Pick the **weight** and the **height** in pixels: the letter size follows from the height, or set it yourself.
4. Pick the **characters**: all of Latin-1 (æøå, é, ü …), only prices (`0-9 , - . :`), or the characters you write. Fewer characters make a smaller file.

The preview shows your sample text pixel for pixel as the label will. It also says when tall letters such as `Å` are cut at the top (as in the built-in fonts; make it taller or the letters smaller if that matters). **Send to the AP** puts it on the AP; **Save the font file** keeps a copy (JSON) you can add to another AP with **Add a font file…**.

Use a font whose licence lets you do this. Fonts from Google Fonts, such as Inter (in `tag/fonts/src`), are under the SIL Open Font License. Once it is on the AP the font is fixed pixels: the AP and the labels draw it the same, whichever computer made it.

### With Python

`tag/tools/gen_font.py --file` makes the same file with FreeType, as the built-in fonts are made:

```
pip install freetype-py pillow
python tools/gen_font.py --file text28 fonts/src/Inter-SemiBold.ttf 22 28 23
```

The numbers are the FreeType size, the height in pixels and the rows above the baseline. It writes `text28.json` and `text28.png`, a picture of every character. `--chars latin1|price|"0123456789,-kr "` picks the characters, `--source "..."` says where the glyphs come from.

## Put it on the AP

**Make a font…** sends it at once. A saved file goes on with **Add a font file…** under *Font files*, or with the API:

```
curl -X PUT http://openesl.local/api/fonts/text28 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  --data-binary @text28.json
```

The AP holds up to 8 font files of 32 KB together, kept across restarts and updates and in the [backup](Backup-and-restore). One file can be at most 24 KB as sent (about 17 KB on the AP); for a bigger one the AP answers 413: use fewer characters or a smaller height.

## Use it in a layout

In the [Label designer](Label-designer), connect to your AP: its font files are then in the font lists next to the built-in fonts, and the preview draws with them. In a layout file, name the font in `fonts` like a built-in one: `"fonts": ["text28", "medium"]`.

## Change or delete it

Send a font file with the same name again to replace it. Every label whose layout uses it is drawn again with the new one at its next poll, so with many labels on that layout, change it when a round of redraws is fine. If the new font no longer fits a box it is used in (it is taller), the AP turns it down and keeps the old one.

A font file that a layout uses cannot be deleted: change those layouts first, then delete it under *Font files* in the dashboard (or `DELETE /api/fonts/text28`).
