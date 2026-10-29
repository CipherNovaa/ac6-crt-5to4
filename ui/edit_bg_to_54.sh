#!/bin/bash
# Reusable recipe: make a full-screen-background .gfx fill 5:4 (stage->5:4 + image COVER, no stretch).
# Usage: edit_bg_to_54.sh <name_without_ext> <BackImage|element name>
# Proven on 01_virtualbg. Edits stage Ymax 21600->30720 + scales the named placement 1.4222 cover (translateX -8100).
set -u
WS="$(cd "$(dirname "$0")" && pwd)"; F="${AC6_FFDEC:-$WS/tools/ffdec/ffdec.sh}"
mkdir -p "$WS/xml" "$WS/gfx/work"
name="$1"; el="${2:-BackImage}"
"$F" -swf2xml "$WS/gfx/vanilla/$name.gfx" "$WS/xml/$name.xml" >/dev/null 2>&1
python3 - "$WS/xml/$name.xml" "$el" <<'PY'
import sys
p,el=sys.argv[1],sys.argv[2]; s=open(p).read()
s=s.replace('Ymax="21600"','Ymax="30720"')  # stage 16:9 -> 5:4
i=s.index('name="%s"'%el); m=s.index('<matrix',i); e=s.index('/>',m)+2
s=s[:m]+'<matrix type="MATRIX" hasRotate="false" hasScale="true" nRotateBits="0" nScaleBits="23" nTranslateBits="20" scaleX="1.4222" scaleY="1.4222" translateX="-8100" translateY="0"/>'+s[e:]
open(p,'w').write(s)
PY
"$F" -xml2swf "$WS/xml/$name.xml" "$WS/gfx/work/$name.gfx" >/dev/null 2>&1
echo "made 5:4: gfx/work/$name.gfx"
