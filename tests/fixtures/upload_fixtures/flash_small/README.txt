ScaleFX Upload Fixture — flash_small
====================================

Structured test payload that mirrors the shape of an on-device config set.
Uploaded as a batch, it should reproduce this directory tree verbatim on the
target filesystem under the remote cwd you chose.

Layout:
  README.txt               (this file)
  config/app.yaml          application-level config
  config/profile.yaml      pilot/session profile
  config/overrides/        environment overlays
  data/points.csv          sample dataset
  data/samples/*.json      decoded samples
  scripts/*.sh             lifecycle scripts

After upload, verify with `file.tree <target> /upload_test`.
