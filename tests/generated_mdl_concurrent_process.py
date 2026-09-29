import pathlib
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="prism-generated-concurrent-") as cache:
    processes = []
    failures = []
    try:
        for _ in range(8):
            processes.append(subprocess.Popen([str(exe), "--writer", cache],
                                              stdout=subprocess.PIPE,
                                              stderr=subprocess.PIPE, text=True))
        for process in processes:
            try:
                stdout, stderr = process.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
                failures.append(f"timeout\n{stdout}{stderr}")
                continue
            if process.returncode:
                failures.append(f"exit={process.returncode}\n{stdout}{stderr}")
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait()
    if failures:
        raise RuntimeError("concurrent process publication failed:\n" +
                           "\n".join(failures))
