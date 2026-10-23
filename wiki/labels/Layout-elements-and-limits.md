# Layout elements and limits

What a layout made in the [Label designer](Label-designer) can hold.

## Elements

- **Text fields:** up to six per layout, sent by your software with each update. Six fonts, aligned left, centre or right, up to three lines, in black, white, inverted or highlighted. Text that does not fit can drop to a smaller font or be cut with "…".
- **Fixed text** that is part of the layout, such as "inkl. mva".
- **Boxes** in black or white, filled or only a border, with round corners if you like. A black area with white text is a black box with white text on it, or inverted text.
- **Lines:** straight or slanted, 1 to 16 px wide. A straight line is exactly as thick as its width, with square ends; a slanted line has round ends.
- **Icons:** small pictures, made from a PNG with the Icon button in the designer. Dark pixels are drawn, up to 128 px.
- **Barcode:** one per layout, Code 128 or EAN-13, from a field.
- **QR code:** one per layout, from a field, for example a link to the product. The box includes a white border (the quiet zone) of 2 modules on every side, drawn white even on a black background, so phones can read it. The box must be at least 25 px wide and high (21 modules of 1 px plus the border). The code grows with the text: if the text is too long for the largest code the box holds, the AP refuses the update with `422`, and the designer says so under the preview.

The AP does not calculate anything, such as VAT: send both prices as fields.

## Limits

- Up to **16 layouts** per label model, in one layout file. The built-in `price` and `text` can always be used as well.
- The text in the fields: about 90 characters in all per label, after æøå and the like are converted to Latin-1.
- The AP limits how many lines of text, boxes, lines and icons one layout can draw, and how big it is when compiled. The status bar under the preview in the designer shows how much of each the layout uses, counted the same way as the AP does.

When the AP cannot draw a layout, it refuses the file with `422` and the reason, the same message the designer shows under the preview.

## The built-in layouts

| Layout | Fields |
|---|---|
| `price` | name, price, unit price; `price_highlight` draws the price inverted |
| `text` | up to four lines; `frame` draws a frame around the screen |
