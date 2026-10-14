# Tag firmware for ZBS243. All logic is in tools/build.py, so the build works
# the same with and without make (on Windows without make: run python tools/build.py <target>).

PYTHON ?= python

.PHONY: all fw test preview fonts clean

all: test fw

fw:
	$(PYTHON) tools/build.py fw

test:
	$(PYTHON) tools/build.py test

preview:
	$(PYTHON) tools/build.py preview

fonts:
	$(PYTHON) tools/build.py fonts

clean:
	$(PYTHON) tools/build.py clean
