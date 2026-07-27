.PHONY: docs docs-check

docs:
	python3 tools/build_docs.py

docs-check:
	python3 tools/build_docs.py --check
