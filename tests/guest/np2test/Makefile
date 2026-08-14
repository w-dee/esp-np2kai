# SPDX-License-Identifier: BSD-2-Clause

ROOT := $(abspath ../../..)
LAYOUT := $(ROOT)/tests/guest/np2test/layout.json
OUTDIR := $(ROOT)/build/guest/np2test
IMAGE := $(OUTDIR)/np2test-fd1232.image

export LC_ALL := C
export TZ := UTC

.PHONY: image verify check test

image:
	mkdir -p $(OUTDIR)
	python3 $(ROOT)/tools/guest/build_np2test.py --layout $(LAYOUT) --output $(IMAGE)

verify: image
	python3 $(ROOT)/tools/guest/verify_np2test.py --layout $(LAYOUT) --image $(IMAGE) --sha256 $(IMAGE).sha256

check: verify

test:
	python3 $(ROOT)/tools/guest/test_np2test.py
