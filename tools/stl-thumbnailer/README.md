# STL thumbnail companion

`folio-stl-thumbnailer INPUT OUTPUT SIZE` makes a transparent PNG without starting a window or initializing a graphics backend. Input and output paths are absolute; SIZE is 1–1024 pixels. The output must not already exist. The tool is built with Folio's existing C++/Qt dependencies and runs in the thumbnail helper session, outside the UI process.

The renderer scans a bounded input snapshot twice, fitting an orthographic camera before rasterizing both sides with a depth buffer and two-by-two coverage samples. It allocates no vertex graph. The camera, raster equations and stepped OKLab palette are adapted from [Flea](https://github.com/thisisgm/flea/tree/cde415617459de2cb452bee14b7699e82a4bdd9b/src/ui_logic_pv4), Copyright (c) 2026 GM, under the [MIT license](LICENSE).

Input is limited to 128 MiB, two million triangles and finite coordinates within 1e9 units. ASCII accepts ordinary whitespace, CRLF and scientific notation. Binary headers starting with `solid` are identified by their count and size. Degenerate geometry, malformed input, unsupported color extensions and excessive raster work are declined. The work bound counts at most 128 million covered bounding-box samples; it never publishes a partially rendered image.

Folio keeps an explicit user thumbnailer first. Otherwise it tries its companion executable, then the selected system thumbnailer if the companion declines or is absent. Both attempts share the same job deadline and session cleanup. Successful images use the existing shared thumbnail cache. Failure markers include the effective CPU/fallback route so installation or replacement of a provider permits another attempt.

Packagers should ship `build/folio-stl-thumbnailer` beside `build/folio`, and this directory's `LICENSE` as an additional license notice. No system `.thumbnailer` file is registered: Folio chooses its bundled companion deliberately, without depending on enumeration order between competing system thumbnailers.
