# RinImage

RinImage is the bounded, caller-buffer image probe and first-frame decode API
used by RinOS userland. It converts accepted inputs into numeric
`0xAARRGGBB` pixel words. The format enum describes format identifiers; it does
not promise complete format compatibility.

## Supported API

The public C API is in `include/rinimage/decoder.h` and `image.h`:

- `rin_image_probe` validates a supported input and reports its dimensions,
  format, frame count, and static/animated kind without allocating.
- `rin_image_decode` and `rin_image_decode_cancellable` decode the first
  frame into caller-owned ARGB8888 storage. GIF and WebP require caller-owned
  scratch storage.
- `rin_image_decode_png` is the PNG-only bounded entry point.
- `rin_image_decode_resource` reads a catalogued image through the caller's
  path-read callback. The codec itself does not open files or resolve paths.
- `rin_image_decode_limits_default`, `rin_image_validate_decode`, and frame
  validation functions provide the common admission contract.

## Supported decoder profiles

| Format | Implemented profile | Deliberate boundary |
| --- | --- | --- |
| PNG | Static PNG; color types 0, 2, 3, 4, and 6; legal 1/2/4/8/16-bit combinations; Adam7; palette and `tRNS`; chunk CRC validation. | APNG animation is not decoded. Ancillary metadata is not exposed as an image color-management or metadata API. |
| JPEG | 8-bit baseline sequential DCT (SOF0), grayscale or three components, with 4:4:4, 4:2:2, or 4:2:0 sampling. | Progressive SOF2, other JPEG processes, precision, component counts, and sampling layouts are not supported. |
| GIF | GIF87a/GIF89a structure and LZW; probe reports frame count; decode returns the first image descriptor only, including interlaced first frames. | This is not an animation player: later frames, timing, disposal compositing, and animation controls are not returned. Transparent first-frame pixels leave the logical-screen background in the opaque output. |
| WebP | VP8 lossy and VP8L lossless payloads, VP8X container metadata needed by the decoder, supported `ALPH` modes, and first `ANMF` frame. | Only the first animation frame is decoded. The API does not expose a complete animation timeline or ICC/EXIF/XMP metadata contract. |
| BMP | Windows DIB header size 40 bytes or greater, uncompressed `BI_RGB`, 24- or 32-bit pixels, top-down or bottom-up rows. | Palettes, bitfields, RLE, and other compression modes are unsupported. For 32-bit input the stored fourth byte is copied as alpha. |
| ICO/CUR | First directory entry only; embedded PNG or uncompressed 24/32-bit DIB payload. | Other entries, compressed DIBs, and other embedded formats are unsupported. |
| TGA | Unmapped, uncompressed true-color image type 2, 24 or 32 bits per pixel; both row origins are handled. | Color maps, RLE, grayscale, and other TGA image types are unsupported. |
| PPM | P3 ASCII and P6 binary RGB with `maxval` exactly 255. | Other Netpbm variants, bit depths, and max values are unsupported. |

`RinImageFormat` also has `UNKNOWN`; it is not decodable. Successful probe of
an animated GIF or WebP does not mean that the decode API returns every frame.

## Ownership and concurrency

The caller owns encoded input, output pixels, limits, and scratch buffers for
the duration of each call. Decoding publishes a complete frame only on
success; the probe output is cleared on failure. `rin_image_decode_resource`
uses a caller-provided read callback and caller-owned source storage. The
common model keeps no image or filesystem handles after return.

Independent calls may use independent buffers concurrently. Do not share a
mutable output or scratch buffer between active calls. The WebP dependency
also exposes process-global last-error diagnostics; callers that need reliable
diagnostics must serialize WebP calls and read the diagnostic immediately
after the failing call.

## Limits and errors

The default common limits are 64 MiB encoded input, 4096 by 4096 dimensions,
16,777,216 pixels, 1024 reported frames, and 64 MiB canonical output. A
consumer may lower these values. Increasing them does not remove codec-local
limits: JPEG and GIF each cap input at 64 MiB and decoded pixels at 16,777,216;
PNG caps input at 64 MiB, either dimension at 4096, and inflated raw data at
256 MiB. The first-frame contract still applies when a larger frame-count
limit is supplied.

`RinImageStatus` distinguishes invalid arguments, unsupported profiles,
resource limits, arithmetic overflow, malformed data, cancellation, timeout,
authorization, and unavailable resource service. Codec errors are normalized
to this common status; the API does not promise that every codec's internal
failure detail is preserved.

## Security

Treat every encoded byte as untrusted. Pass explicit limits suitable for the
calling surface, provide buffers that match those limits, and use the
cancellation-aware entry point when the owner has a cancellation source.
Failure clears the bounded output/scratch prefix; callers must not treat a
failed buffer as a partial image. The common wrapper bounds allocation and
output admission, but individual codecs may allocate temporary memory, so
this API does not claim zero-allocation or a universal CPU deadline.

The parent repository's sanitizer CI builds a deterministic seed corpus for
PNG, JPEG, GIF, WebP, BMP, ICO/CUR, TGA, and PPM and fuzzes the common probe and
decode path. That target caps input at 1 MiB, dimensions at 1024 by 1024,
canonical output at 4 MiB, process RSS at 512 MiB, and each fuzz input at two
seconds. These host-fuzz limits complement the caller-selected production
limits; they do not add a wall-clock deadline to a production decode call.

## ABI, build, and tests

The C headers and their struct layouts are the public source interface. This
repository does not currently declare a separately versioned binary ABI;
layout changes require coordinated updates to RinOS consumers. The codec
sources are integrated by the RinOS parent build with `ringif`, `rinjpeg`,
`rinpng`, and `rinwebp`. This repository has no standalone build or test
target, so a parent build/test result must be reported separately.
