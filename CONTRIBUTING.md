# Contributing

Thanks for helping shape `dgm2xyz`.

## Current Focus

The project is at the documentation and planning stage. The most useful contributions right now are:

- Representative DXF files containing the target point block objects.
- Exact expected `.xyz` output for those files.
- Notes about required coordinate precision, decimal separators, encodings, and file naming.
- Feedback on the proposed architecture and GUI workflow.

## Development Principles

- Keep the application small and focused.
- Prefer simple, testable converter code over GUI-bound logic.
- Add tests around real DXF parsing behavior before broadening support.
- Avoid changing input files.
- Keep output deterministic.

## Pull Requests

Before opening a pull request:

1. Keep changes focused.
2. Update documentation when behavior changes.
3. Add or update tests for converter logic when implementation exists.
4. Describe sample files used for validation, without committing private or licensed CAD data.

## Sample Files

Do not commit confidential, customer-owned, or license-restricted CAD files. If sample files are needed, use minimal synthetic drawings or document how to obtain public examples.
