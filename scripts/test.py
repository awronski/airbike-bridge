"""Run native sanitizer tests, isolated API/UI tests and embedded JS syntax checks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    subprocess.run(command, cwd=ROOT, check=True)


def main():
    with tempfile.TemporaryDirectory(prefix="airbike-tests-") as directory:
        temporary = Path(directory)
        for source in sorted((ROOT / "tests").glob("*_test.cpp")):
            print("Testing " + source.name, flush=True)
            binary = temporary / source.stem
            run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(ROOT / "include"),
                 str(source), "-o", str(binary)])
            run([str(binary)])
        run([sys.executable, "-B", "-m", "unittest", "discover", "-s", "tests", "-p", "*_test.py", "-v"])
        run(["node", "--test", *map(str, sorted((ROOT / "tests").glob("*_test.cjs")))])
        for page in sorted((ROOT / "web").glob("*.html")):
            script = temporary / (page.stem + ".js")
            script.write_text("\n".join(re.findall(r"<script[^>]*>(.*?)</script>", page.read_text(), re.S)))
            run(["node", "--check", str(script)])
        for script in sorted((ROOT / "web").rglob("*.js")):
            run(["node", "--check", str(script)])
    print("All native, API, UI and JavaScript syntax checks passed.")


if __name__ == "__main__":
    main()
