# Every toolchain CI uses, in one image, for tools/check.py --docker (Windows, macOS, or a
# Linux without the tools): ESP-IDF v5.5.5 from Espressif's image, plus SDCC, Emscripten,
# Node and the Python packages from setup.sh.
FROM espressif/idf:v5.5.5

# Copy everything needed for setup.sh into /opt/openesl/
COPY requirements-dev.txt /opt/openesl/requirements-dev.txt
COPY tools/dev/setup.sh /opt/openesl/setup.sh

# Through the image's entrypoint, so the Python packages go into ESP-IDF's Python, which is
# the python3 that check.py runs with
RUN OPENESL_TOOLS=/opt/openesl /opt/esp/entrypoint.sh bash /opt/openesl/setup.sh && chmod -R a+rX /opt/openesl

ENV OPENESL_TOOLS=/opt/openesl
