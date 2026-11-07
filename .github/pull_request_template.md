<!-- Target: development. See CONTRIBUTING.md. -->

## Summary

<!-- What changed, shown with something small: pseudocode, a call tree, a file tree, a Mermaid diagram or the few lines of the diff that matter. -->

## Evidence

<!-- Before and after: screenshots or rendered tag images for visual changes, test or serial output for firmware. -->

- [ ] `python tools/check.py` passes locally (steps run: )
- [ ] Docs and the wiki pages in `wiki/` describe the change (or it changes nothing users see)
- AP image size change per board, if the AP firmware changed (the "AP flash budget" table in the CI run's summary): <!-- C6 +1.2 KB, C6-LCD +1.2 KB, S3 +1.5 KB, C5 +1.4 KB -->
- Hardware tested on: <!-- e.g. 2.9" tag, 1.54" tag, ESP32-C6 AP, or "not tested on hardware" -->

## Merge danger

- Door: <!-- one-way (flash layout, stored settings, radio protocol, OTA, bootloader, breaking API) or two-way (UI, docs, tools) -->
- Blast radius: <!-- what breaks if this is wrong, e.g. tags stop checking in, AP loses settings, only the dashboard -->
