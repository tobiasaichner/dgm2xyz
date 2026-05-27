# Project Requirements

## Purpose

`dgm2xyz` converts supported point data from DXF drawings into plain `.xyz` text files.

## Functional Requirements

- Accept one or more `.dxf` files by drag and drop into the desktop GUI.
- Accept `.dwg` files by drag and drop when the application is built with GNU LibreDWG.
- Open one or more `.dxf` files through a file picker as a secondary input method.
- Detect supported point block objects.
- Extract point coordinates from each supported object.
- Write one `.xyz` file beside each input drawing.
- Keep the source `.dxf` unchanged.
- Show a concise success or failure result for each processed file.

## Open Functional Questions

- Exact block names that represent points.
- Where the height value is stored: block insertion Z, attribute value, text object, or another entity.
- Required output columns and ordering.
- Required coordinate precision and decimal separator.
- Handling of duplicate points.
- Handling of units and coordinate reference systems.
- Whether subfolders should be processed recursively.
- Whether batch conversion should continue after one file fails.

## Non-Functional Requirements

- Small install footprint.
- Fast startup.
- Responsive GUI during batch conversion.
- Deterministic output.
- Useful errors for unsupported files or missing point blocks.

## Development Requirements

- Build with CMake and a C++20 Windows compiler.
- Keep the GUI layer native Win32 unless there is a strong reason to revisit the decision.
- Keep converter behavior testable without starting the GUI.
- Build DWG support with GNU LibreDWG when available.

## Future Requirements

- CLI mode for scripting, if useful after the GUI workflow is stable.
