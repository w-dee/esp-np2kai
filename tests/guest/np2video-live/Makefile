# SPDX-License-Identifier: BSD-2-Clause

ROOT := $(abspath ../../..)
LAYOUT := $(ROOT)/tests/guest/np2video-live/layout.json
OUTDIR := $(ROOT)/build/guest/np2video-live
IMAGE := $(OUTDIR)/np2video-live-fd1232.image
BUILDER := $(ROOT)/tools/guest/build_np2video_stage2.py
TEST := $(ROOT)/tools/guest/test_np2video_live.py

export LC_ALL := C
export TZ := UTC

.PHONY: image test syntax-check reproducibility-check

image:
	mkdir -p $(OUTDIR)
	python3 $(BUILDER) --layout $(LAYOUT) --output $(IMAGE)

test:
	python3 $(TEST)

syntax-check:
	python3 -m py_compile $(BUILDER) $(TEST)

reproducibility-check:
	python3 $(TEST) --reproducibility
