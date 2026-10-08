#!/usr/bin/env python3
"""Recursively embeds every file under a directory into a C++ header as a
table of {url_path, mime_type, content} entries, so the server can serve the
whole frontend from memory without any files on disk at runtime.

Adding a new frontend file (ui/js/foo.js, ui/foo.css, ...) requires no
changes here or in main.cpp's route table — just drop it under ui/ and
reconfigure so CMake's file(GLOB_RECURSE ...) picks it up.

Text files (html/css/js/svg/...) are embedded as raw string literals so the
generated header stays readable and diffable. Anything else (fonts, images)
is binary and can't be represented that way — it's embedded as a byte array
instead. That makes the table dynamically initialised rather than constexpr
(a string_view over a byte array needs a reinterpret_cast), which costs
nothing measurable: it's built once at startup.

Each entry also carries an ETag: a hash of exactly the bytes served, so a
browser that already has a file revalidates it with a 304 and no body, and
any change to the file (any rebuild that changes it) changes its ETag.

Usage: embed_files.py <ui_dir> <output_header>
"""
import hashlib
import pathlib
import sys

MIME_TYPES = {
    '.html':  'text/html; charset=utf-8',
    '.css':   'text/css; charset=utf-8',
    '.js':    'application/javascript; charset=utf-8',
    '.svg':   'image/svg+xml',
    '.json':  'application/json',
    '.txt':   'text/plain; charset=utf-8',
    '.woff2': 'font/woff2',
    '.ttf':   'font/ttf',
    '.png':   'image/png',
    '.ico':   'image/x-icon',
}
DEFAULT_MIME = 'application/octet-stream'

# Suffixes embedded as raw strings. Everything else is treated as binary.
TEXT_SUFFIXES = {'.html', '.css', '.js', '.svg', '.json', '.txt'}


def text_lead(url_path: str) -> str:
    # The newline after a raw string's opening delimiter is cosmetic, except
    # that XML forbids anything before an <?xml ?> declaration.
    return '' if url_path.endswith('.svg') else '\n'


def etag(served: bytes) -> str:
    # A strong ETag, quoted as HTTP requires; 128 bits of SHA-256 is plenty
    # to tell one version of a file from another.
    return '"' + hashlib.sha256(served).hexdigest()[:32] + '"'


def main() -> None:
    ui_dir  = pathlib.Path(sys.argv[1])
    out_path = pathlib.Path(sys.argv[2])

    files = sorted(p for p in ui_dir.rglob('*') if p.is_file())

    entries = []  # (url_path, mime, kind, payload, etag) — kind is 'text' or 'bin'
    for i, path in enumerate(files):
        url_path = '/' + path.relative_to(ui_dir).as_posix()
        mime     = MIME_TYPES.get(path.suffix, DEFAULT_MIME)

        if path.suffix in TEXT_SUFFIXES:
            content = path.read_text(encoding='utf-8')
            # Each file gets its own index-numbered raw-string delimiter so
            # per-file delimiters can't collide with each other; still verify
            # none of the delimiters below appear in the content itself.
            delim = f'EMBED_{i}'
            if delim in content:
                raise ValueError(f'{path}: raw-string delimiter {delim!r} appears in file content')
            served = (text_lead(url_path) + content).encode('utf-8')
            entries.append((url_path, mime, 'text', (delim, content), etag(served)))
        else:
            data = path.read_bytes()
            entries.append((url_path, mime, 'bin', (i, data), etag(data)))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open('w', encoding='utf-8') as f:
        f.write('#pragma once\n')
        f.write('#include <string_view>\n')
        f.write('// Auto-generated from ui/ by cmake/embed_files.py — do not edit\n\n')
        f.write('struct EmbeddedFile {\n')
        f.write('    std::string_view path;\n')
        f.write('    std::string_view mimeType;\n')
        f.write('    std::string_view content;\n')
        f.write('    std::string_view etag;\n')
        f.write('};\n\n')

        for _, _, kind, payload, _ in entries:
            if kind == 'bin':
                idx, data = payload
                f.write(f'static const unsigned char EMBEDDED_BIN_{idx}[] = {{\n')
                for off in range(0, len(data), 24):
                    chunk = data[off:off + 24]
                    f.write('    ' + ','.join(f'0x{b:02x}' for b in chunk) + ',\n')
                f.write('};\n')

        f.write('\nstatic const EmbeddedFile EMBEDDED_FILES[] = {\n')
        for url_path, mime, kind, payload, tag in entries:
            quoted_tag = tag.replace('"', '\\"')
            if kind == 'text':
                delim, content = payload
                f.write(f'    {{"{url_path}", "{mime}", R"{delim}({text_lead(url_path)}{content}){delim}", '
                        f'"{quoted_tag}"}},\n')
            else:
                idx, data = payload
                f.write(f'    {{"{url_path}", "{mime}", std::string_view('
                        f'reinterpret_cast<const char*>(EMBEDDED_BIN_{idx}), {len(data)}), "{quoted_tag}"}},\n')
        f.write('};\n')


if __name__ == '__main__':
    main()
