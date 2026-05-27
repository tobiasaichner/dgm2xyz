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

The GUI should stay simple:

- File picker or drag and drop.
- Processing list with status per file.
- Output summary.
- Settings only when they are required for real workflows.

### Converter Core

The converter core should:

- Accept input file paths and conversion options.
- Return structured results and diagnostics.
- Avoid GUI dependencies.
- Be covered by unit and fixture-based tests.

### CAD Reader Adapter

Use an adapter around the selected DXF library so parser-specific details do not spread through the application.

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
- Lightweight native GUI, likely Qt Widgets or another small native toolkit after evaluation.
- Parser choice based on real sample files and licensing constraints.

No implementation dependency has been selected yet.
