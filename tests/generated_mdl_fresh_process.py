import os
import pathlib
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="prism-generated-mdl-") as cache:
    env = os.environ.copy()
    env["PRISM_CACHE_DIR"] = cache
    def run(color, disabled=False):
        child_env = env.copy()
        if disabled:
            child_env["PRISM_DISABLE_GENERATION_CACHE"] = "1"
        result = subprocess.run([str(exe), "--probe", *map(str, color)], env=child_env,
                                text=True, capture_output=True)
        if result.returncode == 77:
            raise SystemExit(77)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        line = next((x for x in result.stdout.splitlines()
                     if x.startswith("persistentProbe=")), None)
        if not line:
            raise RuntimeError("probe output missing: " + result.stdout)
        return line
    first = run((.2, .3, .4))
    second = run((.8, .15, .25))
    reference = run((.8, .15, .25), disabled=True)
    if "persistentProbe=miss" not in first:
        raise RuntimeError("empty cache unexpectedly hit: " + first)
    if "persistentProbe=hit" not in second:
        raise RuntimeError("fresh process did not reuse color template: " + second)
    if first.split("hash=", 1)[1] == second.split("hash=", 1)[1]:
        raise RuntimeError("parameter edit reused stale generated source")
    if second.split("hash=", 1)[1] != reference.split("hash=", 1)[1]:
        raise RuntimeError("persistent hit differs from independent generation")
