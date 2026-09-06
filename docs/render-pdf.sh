#!/bin/bash
# Render a Markdown doc to PDF: markdown-it-py for HTML, docs/doc.css for
# print styling, headless Chrome for the PDF. Relative image paths are
# resolved with a <base> tag, so this can be run from anywhere.
#
#   docs/render-pdf.sh docs/io-map.md docs/io-map.pdf
#
# NB: the markdown-it *CLI* runs in CommonMark mode, which has no tables —
# it silently emits the pipe characters as prose. Hence the API call below
# with the table and strikethrough rules explicitly enabled.
set -e
SRC="$(realpath "$1")"; OUT="$(realpath -m "$2")"
HERE="$(dirname "$(realpath "$0")")"
HTML="${3:-$(mktemp --suffix=.html)}"

"$HERE/../.venv/bin/python" - "$SRC" "$HERE/doc.css" "$HTML" <<'PY'
import os, sys
from markdown_it import MarkdownIt

src, css, out = sys.argv[1], sys.argv[2], sys.argv[3]
md = (MarkdownIt("commonmark")
      .enable("table")
      .enable("strikethrough"))
body = md.render(open(src, encoding="utf-8").read())
base = os.path.dirname(src)
with open(out, "w", encoding="utf-8") as f:
    f.write('<meta charset="utf-8"><base href="file://%s/">\n' % base)
    f.write("<style>\n%s\n</style>\n" % open(css, encoding="utf-8").read())
    f.write(body)
PY

google-chrome --headless=new --disable-gpu --no-sandbox --allow-file-access-from-files \
  --print-to-pdf="$OUT" --no-pdf-header-footer "file://$HTML" 2>/dev/null
echo "wrote $OUT"
