#!/bin/bash
set -euo pipefail

DIR="${OPENESL_TOOLS:-$HOME/.cache/openesl}"
SDCC_VERSION=4.5.0
EMSCRIPTEN_VERSION=3.1.74
EMSCRIPTEN_HASH=c2655005234810c7c42e02a18e4696554abe0352
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

cat > "$EM/em_config" << EOM
LLVM_ROOT = '$DIR/$EM/install/bin'
BINARYEN_ROOT = '$DIR/$EM/install'
NODE_JS = '$NODE'
EOM

# Logic to find requirements-dev.txt: check same dir as script, then ../../
if [ -f "$(dirname "$0")/requirements-dev.txt" ]; then
  REQ_FILE="$(dirname "$0")/requirements-dev.txt"
else
  REQ_FILE="$(dirname "$0")/../../requirements-dev.txt"
fi

if ! python3 -c "import zxingcpp, PIL, yaml, textual, serial, rich" 2> /dev/null; then
  echo "Installing Python dependencies from $REQ_FILE"
  USER_FLAG=$(python3 -c 'import sys; print("" if sys.prefix != sys.base_prefix else "--user")')
  python3 -m pip install $USER_FLAG --quiet -r "$REQ_FILE" 2> /dev/null ||
    python3 -m pip install $USER_FLAG --quiet --break-system-packages -r "$REQ_FILE"
fi

cat > env.sh << EOM
export PATH="$DIR/sdcc-$SDCC_VERSION/bin:$DIR/$EM/install/emscripten${NODE_BIN:+:$NODE_BIN}:\$PATH"
export EM_CONFIG="$DIR/$EM/em_config"
EOM
echo "Tools are in $DIR (source $DIR/env.sh to use them in this shell)"
