#!/bin/bash
# Installs the toolchains CI uses into ~/.cache/openesl (Linux x86-64, WSL included), so
# tools/check.py can build and test everything on this machine. Safe to run again: what is
# there already is kept. The versions are the ones in .github/workflows/.
#
#   SDCC 4.5.0          tag firmware (tag/tools/build.py: another version gives different code)
#   Emscripten 3.1.74   the web page's renderer (web/build.py)
#   Node 22             web tests, only when no node 22 or newer is on PATH
#   zxing-cpp, pillow, pyyaml   host tests (barcode read-back) and the API spec check
#
# ESP-IDF v5.5.5 is not installed here: tools/check.py uses idf.py when it is on PATH
# (ESP-IDF's export.sh / export.ps1), otherwise the espressif/idf:v5.5.5 Docker image.
#
# Afterwards: source ~/.cache/openesl/env.sh, or just run tools/check.py (it looks there).
set -euo pipefail

DIR="${OPENESL_TOOLS:-$HOME/.cache/openesl}"
SDCC_VERSION=4.5.0
EMSCRIPTEN_VERSION=3.1.74
EMSCRIPTEN_HASH=c2655005234810c7c42e02a18e4696554abe0352   # emscripten-releases-tags.json in emsdk
NODE_VERSION=22.22.0

mkdir -p "$DIR"
cd "$DIR"

if [ ! -x "sdcc-$SDCC_VERSION/bin/sdcc" ]; then
  echo "Installing SDCC $SDCC_VERSION"
  curl -sSfL -o sdcc.tar.bz2 \
    "https://sourceforge.net/projects/sdcc/files/sdcc-linux-amd64/$SDCC_VERSION/sdcc-$SDCC_VERSION-amd64-unknown-linux2.5.tar.bz2/download"
  rm -rf "sdcc-$SDCC_VERSION.tmp" && mkdir "sdcc-$SDCC_VERSION.tmp"
  tar xjf sdcc.tar.bz2 -C "sdcc-$SDCC_VERSION.tmp" --strip-components=1
  mv "sdcc-$SDCC_VERSION.tmp" "sdcc-$SDCC_VERSION" && rm sdcc.tar.bz2
fi

# The prebuilt Emscripten that emsdk would install, without emsdk itself
EM="emscripten-$EMSCRIPTEN_VERSION"
if [ ! -x "$EM/install/emscripten/emcc" ]; then
  echo "Installing Emscripten $EMSCRIPTEN_VERSION"
  curl -sSfL -o wasm.tar.xz \
    "https://storage.googleapis.com/webassembly/emscripten-releases-builds/linux/$EMSCRIPTEN_HASH/wasm-binaries.tar.xz"
  rm -rf "$EM.tmp" && mkdir "$EM.tmp"
  tar xJf wasm.tar.xz -C "$EM.tmp"
  mv "$EM.tmp" "$EM" && rm wasm.tar.xz
fi

NODE_BIN=""
if ! command -v node > /dev/null || [ "$(node -p 'process.versions.node.split(".")[0]')" -lt 22 ]; then
  if [ ! -x "node-$NODE_VERSION/bin/node" ]; then
    echo "Installing Node $NODE_VERSION"
    curl -sSfL -o node.tar.xz "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-x64.tar.xz"
    rm -rf "node-$NODE_VERSION.tmp" && mkdir "node-$NODE_VERSION.tmp"
    tar xJf node.tar.xz -C "node-$NODE_VERSION.tmp" --strip-components=1
    mv "node-$NODE_VERSION.tmp" "node-$NODE_VERSION" && rm node.tar.xz
  fi
  NODE_BIN="$DIR/node-$NODE_VERSION/bin"
fi
NODE="$( [ -n "$NODE_BIN" ] && echo "$NODE_BIN/node" || command -v node )"

cat > "$EM/em_config" << EOF
LLVM_ROOT = '$DIR/$EM/install/bin'
BINARYEN_ROOT = '$DIR/$EM/install'
NODE_JS = '$NODE'
EOF

PY_PACKAGES="zxing-cpp==3.1.1 pillow pyyaml pyserial textual"
if ! python3 -c "import zxingcpp, PIL, yaml, serial, textual" 2> /dev/null; then
  echo "Installing $PY_PACKAGES"
  # --user, except in a virtual environment (such as ESP-IDF's, in the Docker image)
  USER_FLAG=$(python3 -c 'import sys; print("" if sys.prefix != sys.base_prefix else "--user")')
  python3 -m pip install $USER_FLAG --quiet $PY_PACKAGES 2> /dev/null ||
    python3 -m pip install $USER_FLAG --quiet --break-system-packages $PY_PACKAGES
fi

cat > env.sh << EOF
export PATH="$DIR/sdcc-$SDCC_VERSION/bin:$DIR/$EM/install/emscripten${NODE_BIN:+:$NODE_BIN}:\$PATH"
export EM_CONFIG="$DIR/$EM/em_config"
EOF
echo "Tools are in $DIR (source $DIR/env.sh to use them in this shell)"
