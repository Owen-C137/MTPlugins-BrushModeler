# Brush Modeler

Brush Modeler is a standalone MTPlugins add-on for Call of Duty: Black Ops III Mod Tools. It converts selected Radiant brushes and patches to XModels and can merge selected XModels.

## Install

Install the complete `BrushModeler` folder into the Mod Tools plugin directory:

`<Black Ops III Mod Tools>\bin\plugins\BrushModeler\`

The folder must include `BrushModeler.dll`, `BrushModeler.ini`, `merge_xmodels.exe`, and `Tools\exportx.exe`. The plugin also writes `BrushModeler.log` beside the DLL when it runs.

No separate Python or Castline installation is required. ExportX and the merge helper are bundled with the add-on. Model lookup uses BO3's `gdtdb/gdt.db` to find the source GDT, then reads the corresponding GDT entry. Make sure the model is loaded in APE so it is present in the asset database. The existing BO3 Mod Tools executables `export2bin.exe` and `gdtdb.exe` are still used to compile/register the generated asset.

Set `cleanExports=false` under `[model]` in `BrushModeler.ini` to retain generated `.xmodel_export` files and staged `queued_source_*.xmodel_bin` files. It defaults to `true`, which removes these generated intermediates after a successful build; failed builds keep them for debugging.

## Build / distribution

The source and Visual Studio project are in this folder. For a source build, install the build-only dependency with `python -m pip install -r requirements-build.txt`, then run `package-release.ps1`. `merge_xmodels.exe` is generated from `merge_xmodels.py` for releases; it is a self-contained helper and does not require Python on end-user machines. `Tools\exportx.exe` is the upstream ExportX executable by DTZxPorter. See `THIRD_PARTY_NOTICES.md` for attribution.

This addon is distributed separately from the MTPlugins host and is intended to be listed through the MTPlugins Index after testing.
