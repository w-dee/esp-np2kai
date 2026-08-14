# Licensing and Source Provenance

No final project license is selected by this initial setup. This is separate
from the provenance and license-evidence review required for imported source.

For every imported component, record and preserve:

- upstream URL
- version, tag, or commit
- applicable license
- copyright notices
- local modifications

Required notices must remain with the applicable source and distribution
materials. Do not copy snippets from sources whose licensing or provenance is
unclear. Keep third-party provenance auditable from the start.

## NP2kai snapshot evidence

The initial NP2kai candidate snapshot is pinned to the commit recorded in
[`third_party/np2kai/import-manifest.json`](../third_party/np2kai/import-manifest.json).
The manifest is the machine-authoritative import definition, including each
file's license-evidence declaration. The generated
[`LICENSE-MAP.md`](../third_party/np2kai/LICENSE-MAP.md) provides the
file-level evidence inventory; the preserved upstream license and notice files
remain under `third_party/np2kai/`.

Each imported file records evidence such as `upstream_mapping`, referenced
`documents`, `source_notice`, and `review_status`. A completed snapshot must
not contain unresolved `needs-review` evidence. This inventory preserves and
organizes upstream evidence; it does not independently reclassify licenses or
provide legal advice. The entire snapshot must not be described as simply
MIT-licensed.

NP2 source itself has not been imported. Any future third-party import requires
the same provenance, notice-preservation, and license-evidence review.
