# Tools

This directory contains small project tooling and lab helpers. Tool versions
are recorded in [`versions.env`](versions.env); that file is informational and
does not activate or modify an environment.

The `env/` area is for read-only environment checks. The `lab/` area is for
future board and validation helpers. No installer, dependency downloader, or
automatic environment configuration belongs in this initial setup.

## NP2kai snapshot tools

[`np2kai/import_np2kai.py`](np2kai/import_np2kai.py) imports a fixed snapshot
from a caller-provided local upstream Git checkout. It is offline and reads
only the pinned Git objects named by the manifest.

[`np2kai/verify_np2kai.py`](np2kai/verify_np2kai.py) verifies the installed
snapshot offline and can compare its bytes and file modes with the pinned
source checkout. The contributor workflow, replacement safety contract, and
future refresh procedure are documented in
[`docs/development-environment.md`](../docs/development-environment.md).
