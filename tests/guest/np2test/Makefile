# SPDX-License-Identifier: BSD-2-Clause

ROOT := $(abspath ../../..)
LAYOUT := $(ROOT)/tests/guest/np2test/layout.json
OUTDIR := $(ROOT)/build/guest/np2test
IMAGE := $(OUTDIR)/np2test-fd1232.image
GOLDEN := $(ROOT)/tests/guest/np2test/golden/np2test-fd1232.image
GOLDEN_SHA := $(ROOT)/tests/guest/np2test/golden/SHA256SUMS
BUILDER := $(ROOT)/tools/guest/build_np2test.py
VERIFIER := $(ROOT)/tools/guest/verify_np2test.py
REPRODUCIBILITY_CHECK := $(ROOT)/tools/guest/check_np2test_reproducibility.py
WORKTREE_CHECK := $(ROOT)/tools/guest/check_np2test_worktree.py
PYTHON_SOURCES := \
	$(BUILDER) \
	$(VERIFIER) \
	$(ROOT)/tools/guest/test_np2test.py \
	$(REPRODUCIBILITY_CHECK) \
	$(WORKTREE_CHECK)

export LC_ALL := C
export TZ := UTC

.PHONY: image verify golden-check candidate-check check test syntax-check reproducibility-check ci-check

image:
	mkdir -p $(OUTDIR)
	python3 $(BUILDER) --layout $(LAYOUT) --output $(IMAGE)

verify: image
	python3 $(VERIFIER) --layout $(LAYOUT) --image $(IMAGE) --sha256 $(IMAGE).sha256

golden-check: image
	cmp $(IMAGE) $(GOLDEN)
	cd $(ROOT) && sha256sum -c $(GOLDEN_SHA)

candidate-check: image verify test
	@echo "candidate validation passed"

check: verify golden-check

test:
	python3 $(ROOT)/tools/guest/test_np2test.py

syntax-check:
	python3 -m py_compile $(PYTHON_SOURCES)

reproducibility-check:
	python3 $(REPRODUCIBILITY_CHECK) \
		--layout $(LAYOUT) \
		--builder $(BUILDER) \
		--verifier $(VERIFIER)

ci-check:
	@set -eu; \
	state_dir=$$(mktemp -d); \
	trap 'rm -rf "$$state_dir"' EXIT; \
	before="$$state_dir/before.json"; \
	after="$$state_dir/after.json"; \
	python3 $(WORKTREE_CHECK) --repo $(ROOT) --snapshot "$$before"; \
	validation_status=0; \
	$(MAKE) --no-print-directory -j1 syntax-check || validation_status=$$?; \
	if [ "$$validation_status" -eq 0 ]; then $(MAKE) --no-print-directory -j1 test || validation_status=$$?; fi; \
	if [ "$$validation_status" -eq 0 ]; then $(MAKE) --no-print-directory -j1 reproducibility-check || validation_status=$$?; fi; \
	if [ "$$validation_status" -eq 0 ]; then $(MAKE) --no-print-directory -j1 check || validation_status=$$?; fi; \
	python3 $(WORKTREE_CHECK) --repo $(ROOT) --snapshot "$$after"; \
	state_status=0; \
	python3 $(WORKTREE_CHECK) --repo $(ROOT) --compare "$$before" "$$after" || state_status=$$?; \
	if [ "$$validation_status" -ne 0 ]; then exit "$$validation_status"; fi; \
	if [ "$$state_status" -ne 0 ]; then exit "$$state_status"; fi; \
	cd $(ROOT) && git diff --check
