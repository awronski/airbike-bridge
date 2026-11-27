"""Embed the standalone page in flash; no filesystem upload is needed."""
from pathlib import Path
import gzip
import re
import hashlib

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
generated = Path(env.subst("$BUILD_DIR")) / "generated"
generated.mkdir(parents=True, exist_ok=True)
# Short reproducible source identity in downloadable diagnostic reports.
fingerprint = hashlib.sha256()
for path in ("platformio.ini", "scripts/embed_web.py"):
    fingerprint.update(path.encode())
    fingerprint.update((project / path).read_bytes())
for folder in ('src', 'include', 'web'):
    for path in sorted((project / folder).rglob('*')):
        if path.is_file():
            fingerprint.update(path.relative_to(project).as_posix().encode())
            fingerprint.update(path.read_bytes())
build_header = '#pragma once\n#define DIAGNOSTIC_BUILD "' + fingerprint.hexdigest()[:16] + '"\n'
build_target = generated / 'diagnostics_build.h'
if not build_target.exists() or build_target.read_text() != build_header:
    build_target.write_text(build_header)
for source, filename, symbol in (
    ("index.html", "web_page.h", "WEB_PAGE"),
    ("setup.html", "setup_page.h", "SETUP_PAGE"),
):
    page = (project / "web" / source).read_text()
    # Bundle local scripts into the flash page, including fallback media. No CDN
    # or additional HTTP routes are needed on the ATOM.
    def embed_script(match):
        path = (project / "web" / match.group(1)).resolve()
        if not path.is_relative_to((project / "web").resolve()):
            raise ValueError("Script must be inside web/")
        script = path.read_text()
        assert '</script' not in script.lower()
        return '<script>\n' + script + '\n</script>'
    page = re.sub(r'<script src="/([^"]+)"></script>', embed_script, page)
    if source == "index.html":
        payload = gzip.compress(page.encode("utf-8"), mtime=0)
        rows = [
            "    " + ", ".join(f"0x{byte:02x}" for byte in payload[i:i+16]) + ","
            for i in range(0, len(payload), 16)
        ]
        header = "#pragma once\nstatic const unsigned char WEB_PAGE_GZIP[] PROGMEM = {\n" + "\n".join(rows) + "\n};\n"
    else:
        assert ')AIRBIKE_HTML"' not in page
        header = f'#pragma once\nstatic const char {symbol}[] PROGMEM = R"AIRBIKE_HTML(' + page + ')AIRBIKE_HTML";\n'
    target = generated / filename
    if not target.exists() or target.read_text() != header:
        target.write_text(header)
env.Append(CPPPATH=[str(generated)])
