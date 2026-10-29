# Label templates

The [Label designer](Label-designer) has a library of ready-made layouts: **+ From template**. They are grouped by category (3D printer, retail and price, signs, home), each drawn on your label model with its sample text, exactly as the AP draws it. Pick one to see its fields, what each means, and whether your layout file still fits the AP with it; **Add** puts it in your file under a name the file does not have yet, with its sample text, and Undo takes it out again.

Every template has the same fields in the same order on every label model, so an automation keeps working when you swap a 2.9" label for a 1.54". When you switch model in the designer with a template's layout selected, it offers to add the same template for the other model.

The library lives in [templates/](https://github.com/mathiasbk/openESL/tree/master/templates) in the repository and is published with the web page at `https://mathiasbk.github.io/openESL/templates/index.json`. The designer on the AP itself reads it from there; without internet it offers the example layouts the AP carries instead.

## Templates are universal

A template must work for anyone, not for one device, shop or product:

- **No fixed brand, model, product, material or person names** in a layout: not "PRUSA", not "PLA Galaxy Black". Every such value is a field that your software or Home Assistant fills in.
- Only generic labels that mean the same to everyone may be fixed text, such as `%`, `°C`, "Nozzle / bed" or "kr".
- Realistic values go in `_sample` only. They are what the preview shows.

Reviews of template pull requests check this.

## Adding a template

A template is one JSON file, `templates/<category>/<id>.json`, and a pull request with only that file. The easiest way to make one: draw the layout in the designer for each model, then copy each layout from **File → Show as JSON…** into the file.

```json
{
  "format": "esl-template",
  "version": 1,
  "id": "room-climate",
  "category": "home",
  "title": "Room climate",
  "description": "Room name, temperature in large digits, humidity and when it was updated.",
  "tags": ["home", "home-assistant", "temperature"],
  "docs": "docs/home-assistant.md",
  "fields": {
    "room": "Room name",
    "temp": "Temperature, digits only (e.g. 21,5)",
    "humidity": "Relative humidity in percent, digits only",
    "updated": "When the values were read, as text"
  },
  "variants": {
    "2.9": {
      "icons": { "drop": { "w": 16, "h": 22, "data": "..." } },
      "layout": {
        "name": "room-climate",
        "description": "...",
        "fields": ["room", "temp", "humidity", "updated"],
        "_sample": ["Living room", "21,5", "45", "Updated 14:05"],
        "elements": [ ... ]
      }
    },
    "1.54": { "layout": { ... } }
  }
}
```

| Key | Rule |
|---|---|
| `format`, `version` | `"esl-template"`, `1` |
| `id` | a-z, 0-9 and `-`, unique in the library, and the file's name |
| `category` | an `id` from `templates/categories.json`, and the file's folder. A new category is a line in that file (id, title, description, order) |
| `title`, `description` | in English, as the rest of the page; the description says what it shows and what it is for |
| `tags` | optional words the search finds it by |
| `docs` | optional: a file in the repository that tells how to fill it, such as `docs/home-assistant.md` |
| `fields` | every field of the layout, in the order clients send them, each with what it means in plain words |
| `variants` | one per label model, by its name in `shared/models.json` (`"2.9"`, `"1.54"`) |
| `layout` | a layout exactly as in a layout file ([docs/protokoll.md, part 3](https://github.com/mathiasbk/openESL/blob/master/docs/protokoll.md)), with `_sample`: one sample text per field |
| `icons` | optional: the pictures the layout uses, as in a layout file. The designer adds them to the file, renamed if the file has another picture by that name |
| `note` | optional: something about this model only, such as "31 spaces in bar fill the frame" |
| `same_as` | instead of `layout`: use another model's variant, for a model with the same width, height and colours |

Rules the build checks (`python web/build.py`, which runs [web/templates.mjs](https://github.com/mathiasbk/openESL/blob/master/web/templates.mjs); CI fails on any of them):

- the keys above and no others, the category exists, the id is unique and matches the file;
- every variant's layout has exactly the template's fields in the same order, the same layout name, and a sample text for every field;
- every variant compiles with the AP's own compiler on its own, within the limits of the smallest AP (an ESP32-C6), and draws its sample text without the AP refusing it;
- a variant only uses colours its model has (`red` only on a model with red);
- `same_as` only points at a model with the same screen;
- a template with the same layout name as one in the example files (`tag/designs/layouts/<model>.json`) has the same fields and elements, so the two do not drift apart.

Keep a template well within one file's budget, so it can be combined with others: the designer shows what it uses (compiled bytes, text lines, boxes, icons), and the check refuses one over 1 KB compiled. Use only the elements and fonts the AP has (`mid`, `price44` and `large` only have digits and ` ,-.:`), and no third-party artwork: draw icons for the project. Choose the field order so a Home Assistant `rest_command` stays simple.

A template-only change builds no firmware and changes no firmware version: the AP does not carry the library.

## A variant for another label model

A layout is drawn for one screen: 296×128 and 200×200 want different arrangements, so there is no automatic scaling. To add a model to a template, add a variant under its name in `variants` with a layout made for that screen and the same fields in the same order. A model with the same screen as one that has a variant can use `"same_as"` instead.

When a new label model is added to `shared/models.json`, `python web/build.py` lists the templates that have no variant for it yet. The designer shows those greyed out with **All models** on, with a link to ask for the missing variant.
