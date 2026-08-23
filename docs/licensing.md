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

### Step 7B.2a P4-NANO display adapter provenance

The production JD9365 adapter in `firmware/components/p4_nano_display/` is a
project-owned, source-derived adaptation of Waveshare
`waveshare/esp_lcd_jd9365_10_1` version `1.0.4`, upstream commit
`e7721dd43e55cd6b10110543e3efa8dca8e3bfe4`, from
`esp_lcd_jd9365_10_1.c`. The original source SPDX Apache-2.0 metadata is
preserved in the adapted source; the package metadata is MIT and is retained
as a separate fact for review. The hidden display-side I2C/backlight block was
removed, and the board-owned `p4_nano_board` service now exclusively owns
GPIO7/GPIO8, panel-control writes, backlight policy, and cleanup. The original
P4-NANO bring-up copy remains untouched. The local header compatibility shim
pins the same managed-component version for offline builds and does not compile
the unsafe upstream constructor.

The project also acknowledges `np2_espresso` as a useful prior-art/reference
source for future integration review; the Step 7B.2a adapter is not claimed to
be copied from that project.
