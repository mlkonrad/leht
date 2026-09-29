# Lucide icons

SVG icons from [Lucide](https://lucide.dev), `lucide-static` 1.48.0, unmodified, fetched from
`https://cdn.jsdelivr.net/npm/lucide-static@1.48.0/icons/<name>.svg`. ISC licence (see
`LICENSE`; the icons Lucide took from Feather are MIT). Both are compatible with AGPL-3.0.

They are compiled into the viewer as Qt resources (`:/leht/icons/<name>.svg`). `ui/src/icons.cpp`
draws them in the palette's text colour, so they follow light and dark themes. A desktop
icon theme's icon wins when it has one under the freedesktop name.

To add one, fetch it from the same version into this folder. `ui/CMakeLists.txt` picks up every SVG
here (a glob), so there is no list to edit.
