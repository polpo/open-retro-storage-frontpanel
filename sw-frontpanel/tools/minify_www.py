#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Preprocess (product-gate), minify, and gzip a single web asset for
embedding in firmware.

Product-specific sections are gated with comment-based preprocessor
directives, mirroring the C `#ifdef CONFIG_PRODUCT_BLUESCSI` gates:

    // #ifdef PRODUCT_BLUESCSI       |   <!-- #ifdef PRODUCT_BLUESCSI -->
    ...BlueSCSI-only JS...           |   ...BlueSCSI-only markup...
    // #else                         |   <!-- #else -->
    ...PicoIDE-only JS...            |   ...PicoIDE-only markup...
    // #endif                        |   <!-- #endif -->

The inactive product's branch is stripped at build time, so the shipped
asset only references endpoints and DOM that exist in that build. Because
the directives live in comments, the un-preprocessed source remains valid,
runnable JS/HTML for development.

Dependencies (htmlmin4, rjsmin, preprocess) are installed via requirements.txt.

Usage: minify_www.py --product {bluescsi,picoide} <input_file> <output_file>
"""

import argparse
import gzip
import io
import sys
from pathlib import Path

# Symbol defined for each product; consumed by #ifdef directives in the assets.
PRODUCT_DEFINES = {
    'bluescsi': 'PRODUCT_BLUESCSI',
    'picoide': 'PRODUCT_PICOIDE',
}


def preprocess_product(src, product):
    """Strip the inactive product's #ifdef branches from a .js/.html asset.

    Required (not cosmetic): without it the asset would ship both products'
    code, referencing endpoints/DOM that don't exist in this build, so a
    missing dependency is a hard error rather than a skip.
    """
    try:
        import preprocess
    except ImportError:
        print("  Error: 'preprocess' not installed - cannot strip product-specific "
              "sections; refusing to emit an asset containing both products.",
              file=sys.stderr)
        sys.exit(1)

    out = io.StringIO()
    # preprocess() detects comment style from the file extension and mutates
    # the defines dict (adds __FILE__/__LINE__), so pass a fresh one each call.
    preprocess.preprocess(str(src), out, defines={PRODUCT_DEFINES[product]: 1})
    return out.getvalue()


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
    parser = argparse.ArgumentParser(
        description="Preprocess, minify and gzip a web asset for embedding.")
    parser.add_argument('--product', required=True, choices=sorted(PRODUCT_DEFINES),
                        help="Target product; selects which #ifdef branches survive.")
    parser.add_argument('input')
    parser.add_argument('output')
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)
    ext = src.suffix.lower()

    orig_size = len(src.read_bytes())

    # Product-gate .js/.html before minifying. Other assets (e.g. the logo
    # SVG) carry no directives and pass through untouched.
    if ext in ('.html', '.htm', '.js'):
        content = preprocess_product(src, args.product)
    else:
        content = src.read_text(encoding='utf-8')

    # Minify based on type
    if ext in ('.html', '.htm'):
        content = minify_html(content)
    elif ext == '.js':
        content = minify_js(content)

    # Gzip the result
    gzipped = gzip.compress(content.encode('utf-8'), compresslevel=9)

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(gzipped)

    final_size = len(gzipped)
    pct = (orig_size - final_size) / orig_size * 100 if orig_size else 0
    print(f"Processed {src.name} [{args.product}]: {orig_size} -> {final_size} bytes ({pct:.1f}% smaller)")


if __name__ == '__main__':
    main()
