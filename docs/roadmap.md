# Roadmap

## Milestone 0: Repository Setup

- Add repository documentation.
- Add GitHub issue and pull request templates.
- Capture open questions about DXF block structure.

## Milestone 1: Sample-Driven Converter Prototype

- Collect minimal representative DXF samples.
- Define expected `.xyz` output.
- Validate whether the internal DXF reader is enough or whether a DXF reader library is needed.
- Implement converter core for the first supported point block type.
- Add automated tests around sample files.

## Milestone 2: Minimal Desktop GUI

- Add drag-and-drop file intake.
- Add file picker fallback.
- Add batch conversion.
- Show per-file status and diagnostics.
- Package a first local build.

## Milestone 3: Hardening

- Improve error messages.
- Handle malformed files safely.
- Add larger-file performance checks.
- Validate output formatting against real workflows.

## Later

- Harden DWG support against real sample files.
- Consider CLI mode.
- Add installer or portable distribution.
