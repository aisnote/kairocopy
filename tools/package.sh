#!/usr/bin/env bash
# package.sh <version> — dist/kairocopy-win64-<version>.zip from the release build (MSYS2 UCRT64 shell)
set -euo pipefail
V=${1:?version, like v0.1.0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build-release}
LIC=${LIC:-/ucrt64/share/licenses}
NAME=kairocopy-win64-$V
OUT=$ROOT/dist/$NAME
rm -rf "$OUT"
mkdir -p "$OUT/licenses"
for e in kairocopy kairocopy-cli; do cp "$BUILD/$e.exe" "$OUT/"; strip "$OUT/$e.exe"; done
cp "$LIC/xxhash/LICENSE" "$OUT/licenses/xxHash-BSD-2-Clause.txt" 2>/dev/null || cp "$LIC/xxhash/COPYING" "$OUT/licenses/xxHash.txt"
cp "$LIC/winpthreads/COPYING" "$OUT/licenses/winpthreads.txt"
cp "$LIC/gcc-libs/COPYING.RUNTIME" "$OUT/licenses/GCC-runtime-library-exception.txt"
cat > "$OUT/LICENSE.txt" <<EOT
kairocopy $V for Windows (64-bit) - binary distribution
Copyright (c) 2026 the kairocopy authors. All rights reserved.

You may use this program. Its source code is at https://github.com/aisnote/kairocopy. It comes with no warranty of any kind.
The third-party components inside it are listed in THIRD_PARTY_NOTICES.txt.
EOT
cat > "$OUT/THIRD_PARTY_NOTICES.txt" <<EOT
Linked into kairocopy.exe and kairocopy-cli.exe:
  SQLite (public domain), xxHash (BSD-2-Clause), the GCC runtime (GPL-3.0 with the GCC Runtime Library Exception),
  winpthreads (MIT and BSD-3-Clause). The license texts are in licenses\.
EOT
cat > "$OUT/README.txt" <<'EOT'
kairocopy - 在 Windows 和移动硬盘之间快速拷贝：先分析再拷贝，拷过的不再拷，中断了接着拷。

kairocopy.exe       窗口程序：左右两栏，选中后按 F5 拷到另一边（F3 先分析）。
                    Tab 切换左右，Space/Insert 逐个标记，Ctrl+A 全选，Backspace 上一级。
kairocopy-cli.exe   命令行（脚本、agent）：kairocopy-cli analyze/copy/status ... --json

拷贝记录在目标盘根目录的 .kairocopy\manifest.db。不会删除或移动源文件。
EOT
python - "$ROOT/dist" "$NAME" <<'PY'
import os, sys, zipfile
dist, name = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(os.path.join(dist, name + ".zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(os.path.join(dist, name)):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, dist).replace(os.sep, "/"))
PY
ls -la "$ROOT/dist/$NAME.zip"
sha256sum "$ROOT/dist/$NAME.zip"
