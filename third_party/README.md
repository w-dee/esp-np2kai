# Third-Party Sources

The initial NP2kai candidate snapshot is vendored under
[`third_party/np2kai/`](np2kai/). It is generated from the explicit
[`import-manifest.json`](np2kai/import-manifest.json) allowlist and is not an
upstream working-tree copy or Git submodule. The vendor-local
[`README.md`](np2kai/README.md) is the human-readable snapshot summary;
`import-manifest.json` is the machine-authoritative import definition.

For NP2kai and every future third-party dependency, review provenance and
license evidence and preserve the upstream URL, version/tag/commit, copyright
notices, and local-modification status. The NP2 source tree itself is not
included in this snapshot.

Preserve required notices and keep provenance auditable. The NP2kai snapshot's
file-level evidence is recorded in
[`LICENSE-MAP.md`](np2kai/LICENSE-MAP.md), and imported bytes are listed in
[`SHA256SUMS`](np2kai/SHA256SUMS). Do not place source code of unclear origin in
this directory.
