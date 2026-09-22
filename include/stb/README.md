# stb image codecs

Vendored from https://github.com/nothings/stb at commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20`.

- `stb_image.h`: PNG and JPEG decoding (other decoders are disabled).
- `stb_image_write.h`: PNG and JPEG encoding.
- `LICENSE`: upstream dual public-domain / MIT license.

The unmodified headers are compiled once, in `src/TargaImage.cpp`.
No additional binary libraries or runtime downloads are required.
