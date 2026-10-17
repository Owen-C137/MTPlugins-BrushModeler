# Brush Modeler 0.4.0

## Changed

- Model merging, mesh cleanup and mirroring now run inside the plugin. `merge_xmodels.exe` and Python are no longer used or shipped; the C++ merge writes byte-identical exports to the 0.3.14 helper (checked by `Tests\RunTests.ps1` on real models).
- New models get bullet collision (`BulletCollisionLOD` "High") by default, since the brushes they replace are removed. The new **Collision** option sets "None" instead. Mirrored copies use the same option.
- Brushes that add no visible faces to the model (failed rebuilds, or only tool faces such as clip) stay in the map instead of being deleted.

## Added

- **Collision** and **Pivot** (center or bottom center) options, and **Skip tool faces**, which leaves caulk, clip, nodraw, portal, trigger and other tool faces out of the model. All are remembered in `BrushModeler.ini`, as is the last asset name.
- `modelscale` on queued models is applied when merging, and carried over to mirrored copies.
- A GDT entry pointing at an `.xmodel_export` is merged directly without ExportX.
- A **Cancel** button for builds as well as model searches; unloading the plugin stops a running build.
- export2bin, ExportX and gdtdb output is written to `BrushModeler.log`, so failures show their cause.

## Fixed

- Brush faces take their material and texture mapping from their own brush. Two brushes in one entity sharing plane points could swap materials, and the lookup was quadratic in the selection size.
- UVs are shifted by whole texture repeats to stay near 0..1, avoiding precision loss far from the map origin. Export positions, normals and UVs keep full float precision, and placements are no longer rounded to 6 significant digits.
- Mirroring a model with more than 65,535 vertices (`NUMVERTS32`) left its positions unmirrored. Mirroring now flips coordinates textually, so no digits change.
- Asset names are checked against every xmodel in gdt.db, not just the target GDT.
- Derived models' skin overrides include every level of the chain, and a changed GDT is re-read instead of using stale cached lookups.
- Staged files go in a per-build folder, removed after a successful build when `cleanExports=true` and kept for debugging after a failure.
- Selection checks no longer write out the whole worldspawn entity on every selection change.
- The Radiant 3D asset refresh only calls its hard-coded addresses on the Radiant build they were taken from.
- Deploying from Visual Studio no longer overwrites the live `BrushModeler.ini`.
- Output folders whose drive letter or folder case differs from the game path are recognised as inside `model_export`.

# Brush Modeler 0.3.14

## Fixed

- Kept the final triangle's trailing object and material sections outside its vertex records when reversing winding, preventing Radiant's mesh converter from rejecting mirrored models.
- Reversed both VERT and VERT32 face records safely.

# Brush Modeler 0.3.13

## Improved

- Mirror now places the generated asset beside the original model in Radiant, using the model bounds and preserving its rotation. The original entity remains in place and the new copy can be undone.
- Accepts ExportX's comma-separated vertex coordinates when reflecting model geometry.

# Brush Modeler 0.3.12

## Added

- Added a separate mirror action for one queued model, with independent X, Y, and Z axis selection. The reflected copy is compiled into a new `_mirror` asset entry in the source model's GDT, leaving the original map entity in place.

# Brush Modeler 0.3.11

## Fixed

- Merged queued models in one helper pass. Previously, every model caused the complete accumulated export to be parsed, optimized, and rewritten again, making large selections progressively slower.
- Kept per-model transform auditing while running mesh cleanup once for the finished combined export.

# Brush Modeler 0.3.10

## Added and improved

- Model lookup now uses BO3's `gdtdb/gdt.db` directly to find the source GDT. The separate ripgrep executable is no longer needed or included.
- Added clearer guidance when a selected model needs to be queued before building a combined model.
- Expanded merge logging with per-model source transforms, placement, and geometry bounds to make transform issues easier to diagnose.

## Fixed

- Preserved individual Radiant placement when merging multiple models, including static models with identity multi-bone skeletons.
- Kept derived-model material overrides when resolving a model through its parent GDT entry.
- Preserved independent skeleton bone names when appending repeated model instances, preventing one instance's bind pose from affecting another.

## Removed

- Removed the bundled ripgrep executable and its license files from the runtime and release package. The database lookup performed similarly in the measured model search and avoids shipping an extra search executable.
