#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Minify and gzip a single web asset for embedding in firmware.
Dependencies (htmlmin4, jsmin) are installed via requirements.txt.

Usage: minify_www.py <input_file> <output_file>
"""

import sys
import gzip
from pathlib import Path


def minify_html(content):
    """Minify HTML content."""
    try:
        import htmlmin
        return htmlmin.minify(content,
                              remove_comments=True,
                              remove_empty_space=True,
                              remove_all_empty_space=False,
                              reduce_boolean_attributes=True)
    except ImportError:
        print("  Warning: htmlmin not installed, skipping HTML minification", file=sys.stderr)
        return content


def minify_js(content):
    """Minify JavaScript content."""
    try:
        import rjsmin
        return rjsmin.jsmin(content)
    except ImportError:
        print("  Warning: rjsmin not installed, skipping JS minification", file=sys.stderr)
        return content


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_file> <output_file>", file=sys.stderr)
        sys.exit(1)

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])

    content = src.read_text(encoding='utf-8')
    ext = src.suffix.lower()

    # Minify based on type
    if ext == '.html':
        minified = minify_html(content)
    elif ext == '.js':
        minified = minify_js(content)
    else:
        minified = content

    # Gzip the result
    minified_bytes = minified.encode('utf-8')
    gzipped = gzip.compress(minified_bytes, compresslevel=9)

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(gzipped)

    orig_size = len(content.encode('utf-8'))
    final_size = len(gzipped)
    pct = (orig_size - final_size) / orig_size * 100 if orig_size else 0
    print(f"Minified {src.name}: {orig_size} -> {final_size} bytes ({pct:.1f}% smaller)")


if __name__ == '__main__':
    main()
