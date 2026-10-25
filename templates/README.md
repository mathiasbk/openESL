# Layout templates

The designer's template library (**+ New → From template…** in the Design tab): one JSON file per
template, `<category>/<id>.json`, with a layout for each tag model, and the categories in
`categories.json`. `python web/build.py` checks them with the AP's compiler and publishes them
with an `index.json` next to the web page (`web/templates.mjs`).

The format, the rules (templates are universal: no fixed brand, product, material or person names)
and how to add a template or a variant for another model: [wiki/labels/Label-templates.md](../wiki/labels/Label-templates.md).
