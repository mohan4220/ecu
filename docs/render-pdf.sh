#!/bin/bash
# Render a Markdown doc to PDF the same way the checked-in PDFs were made:
# markdown-it for HTML, headless Chrome for print. Relative image paths are
# resolved with a <base> tag, so run it from anywhere.
#
#   docs/render-pdf.sh docs/circuit-guide.md docs/circuit-guide.pdf
#
set -e
SRC="$1"; OUT="$(realpath -m "$2")"
BASE=$(dirname "$(realpath "$SRC")")
HTML="${3:-$(mktemp --suffix=.html)}"
{
  echo '<meta charset="utf-8"><base href="file://'"$BASE"'/">'
  echo '<style>body{font-family:DejaVu Sans,sans-serif;max-width:52em;margin:2em auto;line-height:1.5}
  pre{background:#f6f6f6;padding:.8em;overflow-x:auto;font-size:9pt;white-space:pre}
  code{font-family:DejaVu Sans Mono,monospace;font-size:9pt}
  table{border-collapse:collapse;font-size:9pt}td,th{border:1px solid #bbb;padding:3px 6px}
  img{max-width:100%}h1,h2,h3{page-break-after:avoid}</style>'
  "$(dirname "$(realpath "$0")")/../.venv/bin/markdown-it" "$SRC"
} > "$HTML"
google-chrome --headless=new --disable-gpu --no-sandbox --allow-file-access-from-files \
  --print-to-pdf="$OUT" --no-pdf-header-footer "file://$HTML" 2>/dev/null
