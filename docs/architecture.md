# Architecture Notes

## Proposed Shape

Keep the converter core independent from the GUI:

```text
GUI -> application service -> converter core -> CAD reader adapter
                                      |
                                      -> XYZ writer
```

## Components

### GUI

The GUI is a native Win32 desktop shell using Common Controls v6. It should stay simple:

- Drag-and-drop area for one or more supported CAD files.
- File picker as a secondary input method.
- Processing list with status per file.
- Output summary.
- Settings only when they are required for real workflows.

External GUI frameworks are intentionally avoided in the first implementation. The project favors fast startup, small deployment size, and a narrow Windows utility workflow over cross-platform UI reuse.

### Converter Core

The converter core should:

- Accept input file paths and conversion options.
- Return structured results and diagnostics.
- Avoid GUI dependencies.
- Be covered by unit and fixture-based tests.

### CAD Reader Adapter

Use an adapter around DXF parsing so parser-specific details do not spread through the application.

The first implementation uses a small internal ASCII DXF group-code reader for the narrow target case. This avoids early licensing and deployment decisions while real sample files are still unknown.

The first milestone should support DXF only. DWG support should be investigated separately because libraries, licensing, and file compatibility vary significantly.

### XYZ Writer

The writer should be deterministic:

- Stable point order.
- Explicit numeric formatting.
- Predictable line endings.
- No source file mutation.

## Candidate Technology Direction

Recommended starting direction:

- CMake project.
- C++20 converter core.
- Native Win32 GUI.
- Internal DXF reader until real samples justify a third-party parser.

No external implementation dependency has been selected yet.
