# Upload Fixtures

Test payloads for `file.upload-batch` (CLI) and Studio drag-and-drop upload.

## `flash_small/`

Small structured tree — committed to git. Totals well under 1 MB so it fits
on any controller's flash (typical LittleFS partition is 1 MB with ~1 MB
free after config). Nested dirs exercise the mkdir-preserving-structure
path. Mix of YAML / CSV / JSON / shell text so size is realistic without
binary noise.

Usage (with board on COM5, flash):

```bash
app/go/scalefx-cli.exe
> connect COM5 6000000
> init
> file.upload-batch flash /upload_test tests/upload_fixtures/flash_small
```

Verify with `file.tree flash /upload_test`, then clean up:

```bash
> file.delete flash /upload_test
```

## `sd_large/`

Multi-MB payload for SD card testing (stream/windowed mode). **Not committed**
— generate with:

```bash
cd tests/upload_fixtures
go run gen_sd_large.go
```

Produces a ~6 MB tree with a mix of small metadata + multi-MB binary
blobs. Content is deterministic (seeded RNG) so MD5s are stable across
regenerations.

Usage:

```
> file.upload-batch sd /upload_test tests/upload_fixtures/sd_large --stream
> file.tree sd /upload_test
> file.delete sd /upload_test
```

`--stream` selects the windowed upload mode (high throughput, SD only).
