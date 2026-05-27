# dgm2xyz

Small, light desktop GUI tool for converting selected point blocks from DXF files into `.xyz` point lists.

The goal is deliberately narrow: open a supported CAD file, extract point positions from known block point objects, and write a matching `.xyz` file beside the source drawing.

## Status

This repository contains the first native Windows implementation skeleton:

- Native Win32 desktop GUI with drag-and-drop file intake.
- C++20 converter core with deterministic `.xyz` writing.
- Internal ASCII DXF reader for initial `POINT` and `INSERT` extraction.
- Dependency-free core tests.

## Intended Workflow

1. Drag and drop one or more `.dxf` files into the application, or select them with a file picker.
2. Read supported point block objects from each drawing.
3. Extract point coordinates and optional point metadata.
4. Write a `.xyz` file next to the input file.
5. Report skipped files, unsupported objects, and parsing errors clearly.

## Output

The initial target output is a plain text `.xyz` file:

```text
x y z
```

The exact column order, decimal precision, coordinate handling, and optional attributes still need to be confirmed from real input files.

## Design Goals

- Lightweight native desktop GUI.
- Fast startup and low memory use.
- Drag-and-drop file intake as a primary workflow.
- Batch-friendly file processing.
- Clear error reporting for unsupported or malformed drawings.
- Conservative file handling: write output beside the input DXF and do not modify the source drawing.
- Implementation choices should keep future DWG support possible, but DXF is the first target.

## Documentation

- [Project requirements](docs/requirements.md)
- [Architecture notes](docs/architecture.md)
- [Roadmap](docs/roadmap.md)
- [DXF sample notes](docs/dxf-samples.md)
- [Contributing](CONTRIBUTING.md)

## License

No license has been selected yet.

## Development

The current implementation stack is:

- CMake for builds.
- C++20.
- Native Win32 and Common Controls v6 for the desktop GUI.
- No external GUI framework.
- No third-party CAD library yet.

See [architecture notes](docs/architecture.md) for the initial direction.

### Build

From a Visual Studio developer shell or another environment with a C++20 Windows compiler:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```
