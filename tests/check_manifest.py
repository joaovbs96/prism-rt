"""Checks that Windows executables carry a longPathAware application manifest
(resources/windows/prism.manifest). The manifest is embedded as XML text in the
executable's resources, so a byte search finds it. Usage: check_manifest.py exe..."""
import sys


def main(paths):
    failed = False
    for path in paths:
        with open(path, "rb") as f:
            data = f.read()
        ok = b"longPathAware" in data and b"true" in data[data.index(b"longPathAware"):][:64]
        print(("ok   " if ok else "FAIL ") + path)
        failed |= not ok
    return 1 if failed or not paths else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
