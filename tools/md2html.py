#!/usr/bin/env python3
"""
md2html.py - Convert Markdown files to standalone HTML.

Usage:
    md2html.py INPUT.md [OUTPUT.html]

If OUTPUT is omitted, writes to stdout.
Requires: python3 -m pip install markdown
"""

import sys
import os
import markdown


HTML_TEMPLATE = """\
<!DOCTYPE html>
<html lang="{lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
body {{
    max-width: 50em;
    margin: 2em auto;
    padding: 0 1em;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    font-size: 16px;
    line-height: 1.6;
    color: #222;
}}
h1, h2, h3 {{ margin-top: 1.5em; }}
code {{
    background: #f4f4f4;
    padding: 0.15em 0.3em;
    border-radius: 3px;
    font-size: 0.9em;
}}
pre {{
    background: #f4f4f4;
    padding: 1em;
    overflow-x: auto;
    border-radius: 4px;
}}
pre code {{
    background: none;
    padding: 0;
}}
table {{
    border-collapse: collapse;
    margin: 1em 0;
}}
th, td {{
    border: 1px solid #ccc;
    padding: 0.4em 0.8em;
    text-align: left;
}}
th {{
    background: #f0f0f0;
}}
</style>
</head>
<body>
{body}
</body>
</html>
"""


def detect_lang(filepath):
    """Detect language from path (docs/cz/... -> cs, docs/en/... -> en)."""
    parts = os.path.normpath(filepath).replace("\\", "/").split("/")
    for p in parts:
        if p == "cz":
            return "cs"
        if p == "en":
            return "en"
    return "en"


def extract_title(md_text):
    """Extract title from first # heading, fallback to 'mzdisk'."""
    for line in md_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("# ") and not stripped.startswith("## "):
            return stripped[2:].strip()
    return "mzdisk"


def convert(input_path, output_path=None):
    """Convert a single .md file to .html."""
    with open(input_path, "r", encoding="utf-8") as f:
        md_text = f.read()

    body = markdown.markdown(
        md_text,
        extensions=["tables", "fenced_code", "toc"],
        output_format="html5",
    )

    html = HTML_TEMPLATE.format(
        lang=detect_lang(input_path),
        title=extract_title(md_text),
        body=body,
    )

    if output_path:
        os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(html)
    else:
        sys.stdout.write(html)


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(0 if sys.argv[1:] else 1)

    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else None
    convert(input_path, output_path)


if __name__ == "__main__":
    main()
