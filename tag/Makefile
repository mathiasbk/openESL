# Tag firmware for ZBS243. All logic is in tools/build.py, so the build works
# the same with and without make (on Windows without make: run python tools/build.py <target>).

PYTHON ?= python

.PHONY: all fw test host fonts clean

all: test fw

fw:
	$(PYTHON) tools/build.py fw

test:
	$(PYTHON) tools/build.py test

# Tests and previews for every tag model (2.9" and 1.54"), see tools/render_host.py
host:
	$(PYTHON) tools/render_host.py

fonts:
	$(PYTHON) tools/build.py fonts

clean:
	$(PYTHON) tools/build.py clean
