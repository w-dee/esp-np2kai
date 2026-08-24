# SPDX-License-Identifier: BSD-2-Clause

ROOT := $(abspath ../../..)
LAYOUT := $(ROOT)/tests/guest/np2kbdtest/layout.json
OUTDIR := $(ROOT)/build/guest/np2kbdtest
IMAGE := $(OUTDIR)/np2kbdtest-fd1232.image
GOLDEN := $(ROOT)/tests/guest/np2kbdtest/golden/np2kbdtest-fd1232.image
GOLDEN_SHA := $(ROOT)/tests/guest/np2kbdtest/golden/SHA256SUMS
MANIFEST := $(OUTDIR)/manifest.json
TRACKED_MANIFEST := $(ROOT)/tests/guest/np2kbdtest/manifest.json
BUILDER := $(ROOT)/tools/guest/build_np2kbdtest.py
VERIFIER := $(ROOT)/tools/guest/verify_np2kbdtest.py
TEST := $(ROOT)/tools/guest/test_np2kbdtest.py

export LC_ALL := C
export TZ := UTC

.PHONY: image verify golden-check manifest-check test reproducibility-check check

image:
	mkdir -p $(OUTDIR)
	python3 $(BUILDER) --layout $(LAYOUT) --output $(IMAGE) --manifest $(MANIFEST)

verify: image
	python3 $(VERIFIER) --layout $(LAYOUT) --image $(IMAGE) --sha256 $(IMAGE).sha256

golden-check: image
	cmp $(IMAGE) $(GOLDEN)
	cd $(ROOT) && sha256sum -c $(GOLDEN_SHA)

manifest-check: image
	cmp $(MANIFEST) $(TRACKED_MANIFEST)

test:
	python3 $(TEST)

reproducibility-check:
	python3 $(BUILDER) --layout $(LAYOUT) --output $(OUTDIR)/repro-a.image
	python3 $(BUILDER) --layout $(LAYOUT) --output $(OUTDIR)/repro-b.image
	cmp $(OUTDIR)/repro-a.image $(OUTDIR)/repro-b.image

check: verify golden-check manifest-check test reproducibility-check
