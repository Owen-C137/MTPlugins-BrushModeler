# Brush Modeler

Brush Modeler is a standalone MTPlugins add-on for Call of Duty: Black Ops III Mod Tools. It converts selected Radiant brushes and patches to XModels, merges selected XModels into them, and creates mirrored model assets.

## Install

Install the complete `BrushModeler` folder into the Mod Tools plugin directory:

`<Black Ops III Mod Tools>\bin\plugins\BrushModeler\`

The folder must include `BrushModeler.dll`, `BrushModeler.ini` and `Tools\exportx.exe`. The plugin writes `BrushModeler.log` beside the DLL, including the output of every tool it runs.

No Python or Castline installation is needed. ExportX is bundled; BO3's own `export2bin.exe` and `gdtdb.exe` compile and register the generated assets. `merge_xmodels.exe` from 0.3.x is no longer used and can be deleted.

## Build a model

Select brushes and patches, optionally queue selected model entities (**Queue selected models for merge**, then wait for the lookup), and click **Build combined model**. The model is compiled, added to the chosen GDT and placed in the map in place of the selection, as one undo step.

- **Collision**: "Model (LOD0)" gives the new model bullet collision (`BulletCollisionLOD` "High"); "None" leaves it without.
- **Pivot**: the model origin is the middle of the selected brushes' bounds, or the middle of their base. Models-only builds use the models' average origin.
- **Skip tool faces**: leaves caulk, clip, nodraw, portal, trigger, volume and similar faces out. Brushes with no visible faces stay in the map.

Model lookup uses BO3's `gdtdb/gdt.db` to find each model's GDT, then reads its entry, following derived entries and their `skinOverride` material remaps. Make sure the model is loaded in APE so it is present in the asset database. Queued models keep their `angles` and `modelscale`.

## Mirror a model

Select one Radiant model entity and click **Queue selected models for merge**. After its source lookup finishes, select one or more local axes (X is selected by default) and click **Create mirrored model**. Brush Modeler compiles a reflected copy named `<source>_mirror` (adding a numeric suffix if that name already exists), registers it in the source model's GDT, then places it beside the original in the map. Placement uses the model bounds, preserves its rotation and scale, and leaves a small gap along world X. The original stays in place, the new copy is selected, and the placement can be undone in Radiant. The output folder must be inside `model_export`.

## Settings

`BrushModeler.ini`, under `[model]`:

- `cleanExports=true` removes generated `.xmodel_export` files and staged sources after a successful build. Set it to `false` to keep them. A failed build always keeps its staging folder (`_brushmodeler_*` in the output folder) for debugging.
- `collision`, `pivot`, `skip_tool_faces`, `gdt`, `output_dir` and `asset` hold the pane's last choices.

## Build / distribution

The source and Visual Studio project are in this folder. `Tests\RunTests.ps1` builds and runs the unit checks, compares the C++ merge byte for byte with the reference Python helper (`Tests\reference\merge_xmodels.py`, as shipped in 0.3.14) on models from `model_export`, and checks that export2bin accepts the results. It needs Python only for that comparison. `package-release.ps1` runs the tests, builds the DLL and creates the release zip and index entry; the version comes from `Version.h`. `Tools\exportx.exe` is the upstream ExportX executable by DTZxPorter. See `THIRD_PARTY_NOTICES.md` for attribution.

This addon is distributed separately from the MTPlugins host and is intended to be listed through the MTPlugins Index after testing.
