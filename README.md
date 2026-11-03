# RinImage

RinImage is the bounded, caller-buffer image probe and selected-frame decode API
used by RinOS userland. It converts accepted inputs into numeric
`0xAARRGGBB` pixel words. The format enum describes format identifiers; it does
not promise complete format compatibility.

## Supported API

The public C API is in `include/rinimage/decoder.h` and `image.h`:

- `rin_image_probe` validates a supported input and reports its dimensions,
  format, frame count, static/animated kind, and known loop count without
  allocating. A known positive loop count includes the initial play; zero
  means infinite.
- `rin_image_decode` and `rin_image_decode_cancellable` decode the first
  frame. `rin_image_decode_frame_cancellable` selects any GIF or WebP frame
  or APNG frame and returns its delay and composited canvas in caller-owned
  ARGB8888 storage. APNG loop count follows the same contract as GIF/WebP:
  zero means infinite; a positive value includes the initial play. GIF and
  WebP require caller-owned
  scratch storage. The cancellable API passes its predicate through supported
  codec probes and decode loops, including WebP's RIFF, VP8, VP8L, and ALPH
  processing.
- `rin_image_decode_png` is the PNG-only bounded entry point.
- `rin_image_probe_png_cancellable` and
  `rin_image_decode_png_cancellable` pass a caller cancellation predicate into
  the RinPNG deadline API.
- `rin_image_decode_resource` reads a catalogued image through the caller's
  path-read callback. It rejects null or zero-capacity source storage before
  the callback; the codec itself does not open files or resolve paths.
- `rin_image_decode_limits_default`, `rin_image_validate_decode`, and frame
  validation functions provide the common admission contract.

## Supported decoder profiles

| Format | Implemented profile | Deliberate boundary |
| --- | --- | --- |
| PNG | Static PNG and APNG; color types 0, 2, 3, 4, and 6; legal 1/2/4/8/16-bit combinations; Adam7; palette and `tRNS`; chunk CRC validation. APNG frame rectangles are decoded and composited using source/over blend and none/background/previous disposal, with per-frame delay and total play count. | Static `rpng_*` calls reject APNG; select frames through RinImage's indexed API. Ancillary metadata is not exposed as an image color-management or metadata API. |
| JPEG | 8-bit baseline sequential DCT (SOF0), grayscale or three components, with 4:4:4, 4:2:2, or 4:2:0 sampling. Cancellation is polled during marker parsing, MCU rows, and output rows. | Progressive SOF2, other JPEG processes, precision, component counts, and sampling layouts are not supported. |
| GIF | GIF87a/GIF89a structure and LZW; indexed frame decode composites frame rectangles with transparency and disposal methods 2/3, and reports frame delays and Netscape loop count. Cancellation is polled during block scanning, LZW, palette validation, canvas updates, and output rows. | Frame delays are returned as milliseconds. ICC/EXIF/XMP metadata is not exposed. |
| WebP | VP8 lossy and VP8L lossless payloads, VP8X animation metadata, supported `ALPH` modes, and indexed `ANMF` frame decode with blend/disposal composition, frame delays, and loop count. Cancellation is polled during RIFF/frame chunk scans, compressed decode, alpha processing, compositing, and output conversion. | The API does not expose ICC/EXIF/XMP metadata. |
| BMP | Windows DIB header size 40 bytes or greater, uncompressed `BI_RGB`, 24- or 32-bit pixels, top-down or bottom-up rows. | Palettes, bitfields, RLE, and other compression modes are unsupported. For 32-bit input the stored fourth byte is copied as alpha. |
| ICO/CUR | First directory entry only; embedded PNG or uncompressed 24/32-bit DIB payload. | Other entries, compressed DIBs, and other embedded formats are unsupported. |
| TGA | Unmapped, uncompressed true-color image type 2, 24 or 32 bits per pixel; both row origins are handled. | Color maps, RLE, grayscale, and other TGA image types are unsupported. |
| PPM | P3 ASCII and P6 binary RGB with `maxval` exactly 255; P6 accepts a CRLF line terminator before the raster. | Other Netpbm variants, bit depths, and max values are unsupported. |

`RinImageFormat` also has `UNKNOWN`; it is not decodable. The indexed PNG
animation path validates APNG sequence numbers, frame bounds, CRCs, frame
count, and aggregate output/work limits before publishing a composited frame.

## Ownership and concurrency

The caller owns encoded input, output pixels, limits, and scratch buffers for
the duration of each call. Decoding publishes a complete frame only on
success; the probe output is cleared on failure. `rin_image_decode_resource`
uses a caller-provided read callback and caller-owned source storage. The
common model keeps no image or filesystem handles after return.

Independent calls may use independent buffers concurrently. Do not share a
mutable output or scratch buffer between active calls. RinWebP's legacy error
accessors expose only the most recent call on the current thread; callers that
need stable per-call diagnostics should use its caller-owned diagnostic API.

## Limits and errors

The default common limits are 64 MiB encoded input, 4096 by 4096 dimensions,
16,777,216 pixels, 1024 reported frames, and 64 MiB canonical output. Indexed
animation decode applies that ceiling to all canvas frames; the original
frame-zero API budgets its one output frame. A consumer may lower these
values. Increasing them does not remove codec-local
limits: JPEG and GIF each cap input at 64 MiB and decoded pixels at 16,777,216;
PNG caps input at 64 MiB, either dimension at 4096, inflated raw data at 256
MiB, APNG frame count at 4096, and cumulative canvas/frame processing at 64
million pixels. The indexed animation API applies the aggregate frame budget
when a larger frame-count limit is supplied.

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
output admission. Cancellable PNG/JPEG/GIF/WebP paths poll inside their
parsing and decode loops. BMP/TGA/ICO/CUR poll while converting bounded rows,
and PPM polls during header/comment scanning as well as raster conversion.
Cancellation is caller-driven and does not impose a universal CPU deadline or
zero-allocation guarantee.

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

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | Caller-buffer image probing and first-frame decoding to numeric ARGB pixels. |
| Supported API | include/rinimage/decoder.h, image.h, service.h, and thumbnail_cache.h; see Supported API above, including the all-format cancellable decode and PNG-only cancellable probe/decode entries. |
| Unsupported API | Only the explicit per-format profiles above; no full-format, animation-playback, or metadata/color-management claim. |
| ownership | Caller owns encoded input, output pixels, limits, and scratch; resource access uses caller callback. |
| thread-safety | Independent buffers may be used concurrently; shared output/scratch must be serialized. WebP diagnostics are process-global. |
| limits | See limits above for shared input/pixel/output bounds and codec-local limits. |
| errors | RinImageStatus reports malformed, unsupported, limit, overflow, cancellation, authorization, and resource-service failures. |
| ABI stability | Public C source interface without separately versioned binary ABI; coordinated rebuild needed for layout changes. |
| security | Input is untrusted; wrapper bounds admission and the cancellable decode polls within all supported codecs. The caller owns cancellation policy; no universal CPU deadline or zero-allocation guarantee is published. |
| build | Integrated through RinOS parent build; no standalone build target. |
| test | Parent sanitizer CI fuzzes the common probe/decode path; no standalone test target. No tests/builds run for this README update. |
