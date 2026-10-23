# Price lists from CSV

[esl-client](esl-client) sends a whole price list from a CSV file. Unchanged labels cost nothing, so you can run it as often as you like.

```sh
python3 esl_client.py sync prices.csv
python3 esl_client.py sync prices.csv --dry-run     # check and show, send nothing
```

## The file

```csv
mac,layout,name,price,unit
00000123456789A8,price,Lettmelk 1 L,"24,90",kr 24/L
000001C280923B15,price,Kaffe 500 g,"49,90","kr 99,80/kg"
```

- The header row names the columns, in any order. `mac` and `layout` are needed (`--layout price` gives rows without one a layout).
- `highlight` and `frame` are optional (1/0, true/false or empty).
- Every other column is a field, in column order.
- Exports from Excel work as they are: `,`, `;` or tab, UTF-8 or Windows-1252. `24,90` stays `24,90`.

## Item numbers instead of MACs

Your till or ERP knows item numbers, not label MACs. Keep a second file that says which label shows which item:

```csv
mac,item
00000123456789A8,1001
000001C280923B15,1002
```

```sh
python3 esl_client.py sync prices.csv --tags tags.csv
```

The price file then has an `item` column instead of `mac`. One item can be on several labels. `tags.csv` changes only when labels are put up or moved.

## Checks

The files are checked before anything is sent. A bad MAC, a MAC or item twice, or a row without a layout gives every such line (`prices.csv:12: ...`) and sends nothing. When the AP refuses a row, the error names its line too.

## At a set time

Add `--at "2026-10-19 07:00"`, or an `at` column per row: see [Scheduled prices](Scheduled-prices).
