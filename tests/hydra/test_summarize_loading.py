"""Synthetic contract tests for summarize_loading.py."""
import json
from pathlib import Path
import tempfile
import unittest

import summarize_loading as summary


def _measurement(cache, first_frame, converged, snapshot):
    return {
        "cache": cache,
        "repetition": 1,
        "firstFrameSeconds": first_frame,
        "convergedSeconds": converged,
        "processStartToConvergedSeconds": converged + 1,
        "workingSetBytes": 1000,
        "peakWorkingSetBytes": 2000,
        "stats": {
            "snapshotBuildSeconds": snapshot,
            "materialBatchWallSeconds": 4,
            "materialWorkerCpuSeconds": 7,
            "materialNativeJits": 2,
            "meshCount": 3,
            "materialResourceReadSeconds": 0.75,
            "materialResourceBytes": 12288,
            "sceneTraversalSeconds": 0.3,
            "meshConversionSeconds": 0.4,
            "embreeGeometryCommitSeconds": 0.5,
            "embreeSceneCommitSeconds": 0.6,
            "bufferWriteMilliseconds": 1.1,
            "bufferPublicationMilliseconds": 1.2,
            "bufferMapWaitMilliseconds": 1.3,
            "prototypeCount": 2,
            "prototypePathCount": 5,
            "instanceCount": 8,
            "estimatedGeometryBytes": 4096,
        },
    }


class SummarizeLoadingTests(unittest.TestCase):
    def test_separates_cache_groups_and_reports_ranges(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runs = []
            cases = [
                (1, 4, "fresh-renderer", 2.0, 5.0, 0.5),
                (2, 4, "fresh-renderer", 4.0, 9.0, 1.5),
                (3, 4, "process-warm", 0.25, 1.0, 0.1),
                (4, 2, "fresh-renderer", 8.0, 12.0, 3.0),
            ]
            for run, workers, cache, first, converged, snapshot in cases:
                output = root / f"output-{run}"
                output.mkdir()
                (output / "loading.json").write_text(json.dumps([
                    _measurement(cache, first, converged, snapshot),
                    # A repeated host stage must still count only once per process.
                    _measurement(cache, first + 1, converged + 1, snapshot + 1),
                ]), encoding="utf-8")
                runs.append({
                    "scene": "scenes/OpenChessSet.usda", "workers": workers,
                    "run": run, "processSeconds": 30 + run, "exitCode": 0,
                    "cachePolicy": "fresh process; OS cache uncontrolled",
                    "output": str(output),
                    "hostTimings": [{"phase": "stage", "seconds": 6 + run}],
                })
            runs_path = root / "runs.json"
            runs_path.write_text(json.dumps(runs), encoding="utf-8")
            md_path = root / "summary.md"
            result = summary.summarize(runs_path, md_path)

            self.assertEqual(result["processRunCount"], 4)
            self.assertEqual(len(result["groups"]), 3)
            fresh4 = next(g for g in result["groups"]
                          if g["workers"] == 4 and g["cache"] == "fresh-renderer")
            warm4 = next(g for g in result["groups"]
                         if g["workers"] == 4 and g["cache"] == "process-warm")
            self.assertEqual(fresh4["metrics"]["firstFrameSeconds"], {
                "count": 4, "median": 3.5, "min": 2.0, "max": 5.0})
            self.assertEqual(warm4["metrics"]["firstFrameSeconds"]["count"], 2)
            # The stage timing is process-level, not repeated for every renderer sample.
            self.assertEqual(fresh4["hostTimings"]["stage"]["count"], 2)
            self.assertEqual(fresh4["metrics"]["materialCpuToWallRatio"]["median"], 1.75)
            markdown = md_path.read_text(encoding="utf-8")
            self.assertIn("OS filesystem cache state was uncontrolled", markdown)
            self.assertIn("Fresh-renderer and process-warm measurements are grouped separately", markdown)
            self.assertIn("No prior baseline comparison", markdown)
            self.assertIn("Meshes / proto paths / instances", markdown)
            self.assertIn("Material resource reads (s)", markdown)
            self.assertIn("Render-buffer publication (ms)", markdown)
            self.assertIn("Embree scene commit (s)", markdown)
            # Every renderer sample is available in the JSON output as well.
            encoded = json.loads(md_path.with_suffix(".json").read_text(encoding="utf-8"))
            self.assertEqual(encoded["sampleCount"], 8)

    def test_baseline_requires_exact_configuration_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "output"
            output.mkdir()
            (output / "loading.json").write_text(json.dumps([
                _measurement("fresh-renderer", 1, 2, .1)]), encoding="utf-8")
            runs = root / "runs.json"
            runs.write_text(json.dumps([{"scene": "scene.usda", "workers": 1,
                                         "run": 1, "output": str(output)}]),
                            encoding="utf-8")
            current_config, old_config = root / "current.json", root / "old.json"
            current_config.write_text('{"usd":"24.08"}', encoding="utf-8")
            old_config.write_text('{"usd":"23.11"}', encoding="utf-8")
            mismatch = summary.summarize(runs, root / "mismatch.md", baseline_path=runs,
                                         configuration_path=current_config,
                                         baseline_configuration_path=old_config)
            self.assertFalse(mismatch["baselineComparison"]["performed"])
            old_config.write_text('{"usd":"24.08"}', encoding="utf-8")
            matched = summary.summarize(runs, root / "matched.md", baseline_path=runs,
                                        configuration_path=current_config,
                                        baseline_configuration_path=old_config)
            self.assertTrue(matched["baselineComparison"]["performed"])
            self.assertTrue(matched["baselineComparison"]["comparisons"])


if __name__ == "__main__":
    unittest.main()
