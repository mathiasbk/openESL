# Label designer

A **layout** says where each text field goes on the label, in which font, and what fixed text, boxes, lines, icons, barcodes and QR codes are around it. Your software then sends only the text fields and the layout's name.

The AP comes with two layouts: `price` (name, price, unit price) and `text` (up to four lines).

## The label designer

Open the [Design](https://mathiasbk.github.io/openESL/#design) tab. It works in any modern browser.

![The label designer](https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/web-designer.png)

- **Left:** the layouts in the file, and the elements of the one you picked. "+ New" adds an empty layout or a copy of an example, such as the product labels `vare`, `vare-mva` and `vare-priser`.
- **Middle:** the label, with the tools over it (text, fixed text, box, line, icon, barcode, QR code). Drag and resize boxes; they snap to the edges, the centre and each other (hold Alt to turn it off). The bar next to the selected element sets font, alignment and colour, and more under "More".
- **Right:** the layout's name, its fields with the sample text the preview shows, and where the selected element is.
- **Under the label:** how much of the AP's limits the layout uses, and whether the AP will accept it.

Undo and redo with Ctrl+Z and Ctrl+Shift+Z. Arrows, Tab, Delete and Ctrl+D (duplicate) work on the selected box.

**The preview is exact.** It is the AP's own drawing code, built for the browser, so what you see is pixel for pixel what the label shows.

## Getting it onto the AP

- **Send to tag…** in the designer uploads the layout file to the AP and puts the sample text on a label you pick. It goes through the bridge in [esl-client](https://github.com/mathiasbk/openESL/blob/master/tools/esl-client/README.md) (`esl_client.py --serial /dev/ttyACM0 serve`), since a web page may not call the AP on your network directly.
- **On the AP itself:** an AP with room for it (the [ESP32-S3 + ESP32-C6](ESP32-S3-and-C6) and the [ESP32-C5](ESP32-C5)) has the designer too, at `http://openesl.local/dashboard#design`. There it opens the layout files stored on that AP, **Save to AP** writes your changes back, and **File → Reload from the AP** drops changes you have not saved. The head says when something is not saved to the AP yet. It uses the Dashboard's token, and it needs the internet for the preview, which it loads from the web page.
- Or save the file (File menu) and upload it yourself: `esl_client.py layout-put 2.9 layouts.json`, or `PUT /api/layouts/{model}`.

There is one layout file per label model, holding up to 16 layouts. The built-in `price` and `text` can always be used, with or without a file. When you upload a new version, the AP draws every label that uses it again, and each label gets its new screen at its next poll. A layout the AP cannot draw gets a `422` with the reason, the same message the designer shows.

## The file format

The format is described in [docs/protokoll.md, part 3](https://github.com/mathiasbk/openESL/blob/master/docs/protokoll.md). Example files: [tag/designs/layouts](https://github.com/mathiasbk/openESL/blob/master/tag/designs/layouts). What a layout can hold: [Layout elements and limits](Layout-elements-and-limits).
