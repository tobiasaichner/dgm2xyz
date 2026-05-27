# dgm2xyz

Small, light desktop GUI tool for converting selected point blocks from DXF files into `.xyz` point lists.

The goal is deliberately narrow: open a supported CAD file, extract point positions from known block point objects, and write a matching `.xyz` file beside the source drawing.

## Status

This repository is in project setup. No converter implementation exists yet.

## Intended Workflow

1. Select one or more `.dxf` files.
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

The implementation stack has not been committed yet. The current recommendation is a small native application with:

- CMake for builds.
- C++ for the converter core.
- A lightweight desktop GUI toolkit.
- A DXF reader library selected after testing against real sample files.

See [architecture notes](docs/architecture.md) for the initial direction.
