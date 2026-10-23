# Layout elements and limits

What a layout made in the [Label designer](Label-designer) can hold.

## Elements

- **Text fields:** up to six per layout, sent by your software with each update. Four fonts, aligned left, centre or right, up to three lines, in black, white, inverted or highlighted. Text that does not fit can drop to a smaller font or be cut with "…".
- **Fixed text** that is part of the layout, such as "inkl. mva".
- **Boxes** filled in black or white. A black area with white text is a black box with white text on it, or inverted text.
- **Lines:** a box 1 or 2 px wide.
- **Barcode:** one per layout, Code 128 or EAN-13, from a field.

Not in the format: diagonal lines and images. The AP does not calculate anything either, such as VAT: send both prices as fields.

## Limits

- Up to **16 layouts** per label model, in one layout file. The built-in `price` and `text` can always be used as well.
- The text in the fields: about 90 characters in all per label, after æøå and the like are converted to Latin-1.
- The AP limits how many lines of text and boxes one layout can draw, and how big it is when compiled. The status bar under the preview in the designer shows how much of each the layout uses, counted the same way as the AP does.

When the AP cannot draw a layout, it refuses the file with `422` and the reason, the same message the designer shows under the preview.

## The built-in layouts

| Layout | Fields |
|---|---|
| `price` | name, price, unit price; `price_highlight` draws the price inverted |
| `text` | up to four lines; `frame` draws a frame around the screen |
