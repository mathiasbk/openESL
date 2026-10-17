#!/bin/bash
# Cloud development sessions: installs the toolchains so `python3 tools/check.py` builds and tests
# what CI does before anything is pushed. Does nothing on other machines.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

bash "$CLAUDE_PROJECT_DIR/tools/dev/setup.sh"
cat "$HOME/.cache/openesl/env.sh" >> "$CLAUDE_ENV_FILE"

# Docker, for the AP firmware (espressif/idf:v5.5.5); the image is pulled on first use
if command -v dockerd > /dev/null && ! docker info > /dev/null 2>&1; then
  (dockerd > /tmp/dockerd.log 2>&1 &)
fi
