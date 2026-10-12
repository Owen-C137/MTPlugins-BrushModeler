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
