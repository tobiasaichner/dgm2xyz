# DXF Sample Notes

Real sample files are needed before implementation decisions become reliable.

## Needed For Each Sample

- DXF version.
- Target block name or names.
- How X, Y, and Z are represented.
- Whether point number, name, code, or description should be exported.
- Expected `.xyz` output.
- Whether the file can be committed to the repository.

## Sample Matrix

| Sample | Source | Block name | Height source | Expected columns | Can commit? |
| --- | --- | --- | --- | --- | --- |
| Generated DGM point | User screenshot | `FIGX20` | Block insertion Z / `Hoehe` attribute | `x y z` | TBD |

## Known Point Types

### Generated DGM Points

Generated DGM points are represented as block references:

- Block name pattern: `FIGX...`, for example `FIGX20`
- Layer: variable; examples include `Geländemodell_ALS-Punkt`, but DGM detection must not depend on the layer
- Geometry: insertion position X/Y/Z
- Attribute: `Hoehe`

The implementation groups these as `Geländemodell points` in the GUI preview so one checkbox covers matching DGM point blocks across all layers.

## Privacy

Use synthetic or public sample drawings whenever possible. Avoid committing customer or project data.
