# ImageEditing

The existing command input and scripts support `.tga`, `.png`, `.jpg`, and
`.jpeg`. Extensions are case insensitive. Files without an extension retain
the original TGA behavior. Other output extensions are rejected.

With `photo.jpg` in the application's working directory, enter these commands
one at a time, or save them in a script ending with a newline:

```text
load photo.jpg
gray
save result.png
save result.jpg
save result.tga
```

PNG keeps transparency. JPEG uses quality 90 and composites transparency over
white. Saving does not change the current image. Loading an invalid file keeps
the current image; a failed load or save stops that script and prints an error.

The internal image is 8-bit premultiplied RGBA. PNG alpha is preserved exactly,
but RGB in translucent pixels may lose precision during conversion; fully
transparent pixels do not retain hidden colors. PNG/JPEG paths support UTF-8
filenames. The original command parser separates arguments with whitespace,
so use filenames or relative paths without spaces. TGA uses the original
LibTarga path and encoding behavior.

## Painterly rendering (NPR)

`npr-paint` runs the basic circular-stroke method. `npr-paint-advanced` adds
gradient-guided curved strokes, tapered textured brushes, and translucent
layering. Its optional arguments are brush scale (0.5 to 3, default 1) and
random seed (unsigned 32-bit integer, default 1337):

```text
load Images/wiz.tga
npr-paint-advanced 1.7 1337
save my-oil-paint.png
```

These paths assume the project directory is the working directory. In this
workspace's VS Code configuration, the working directory is `P1`; use
`load wiz.tga` there. See the [操作說明](docs/USER_MANUAL.md),
[技術說明](docs/NPR_TECHNICAL.md), and [example script](examples/npr/demo.txt).
The example PNGs are algorithm outputs; capture the application window for
the rubric's three screenshots.

Two additional styles are available: `npr-cartoon [strength]` for cel-style
color bands and ink contours, and `npr-watercolor [brush-scale [seed]]` for
irregular translucent washes, pigment edges, paper grain, and dry-brush detail.
Strength/scale defaults to 1 and accepts 0.5 to 3; watercolor's seed defaults
to 1337. Reload the source image before comparing styles.

## Build

Use the supplied Visual Studio 2019 x64 / FLTK libraries:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Debug --target ImageEditing
```

The PNG/JPEG codecs are vendored in `include/stb`; no additional DLLs or
downloads are needed to build or run the program.

## Verify image formats

The integration tests use Pillow to independently generate and inspect images.
They exercise the actual application's script interpreter:

```powershell
python tests/test_image_formats.py -v
```

Use `--exe PATH` to test a different executable. Tests cover PNG color modes,
alpha, row orientation, JPEG input/output, existing editing operations, TGA
conversion, filename case and Unicode, and load/save failures.
