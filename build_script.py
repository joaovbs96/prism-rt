"""Configure/build PrismRT using the repository's portable CMake presets."""
import argparse
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=("prism-standalone", "prism-hydra"),
                        default="prism-hydra")
    parser.add_argument("--clean", action="store_true",
                        help="clean compiled outputs while retaining configured dependencies")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    build = root / "build"
    if args.clean and (build / "CMakeCache.txt").exists():
        # vcpkg dependencies may be installed below build/. Deleting the whole
        # directory would destroy the dependency installation we intend to reuse.
        subprocess.run(["cmake", "--build", str(build), "--config",
                        "RelWithDebInfo", "--target", "clean"], cwd=root, check=True)
    subprocess.run(["cmake", "--preset", args.preset], cwd=root, check=True)
    subprocess.run(["cmake", "--build", "--preset", args.preset], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
