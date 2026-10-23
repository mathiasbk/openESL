# Label backups

Before anything is erased, the [Flash tag](https://mathiasbk.github.io/openESL/#tag) tab reads the label's whole original firmware and keeps it in the browser, with the barcode printed on the label (read from the original firmware, and editable).

- **Export them:** from the same tab, as a ZIP with every image and a `backups.csv` list, or the list alone.
- **Keep them safe:** they are the only way back to the original firmware. They stay in the browser until the site's data is cleared, so export them now and then.
- **Going back:** write a backup to the label with ZBS_Flasher.

[flash-tui](https://github.com/mathiasbk/openESL/blob/master/tools/flash-tui/README.md) keeps its backups in the `backup/` folder, with a line per label in `backup/flash_log.csv`.
