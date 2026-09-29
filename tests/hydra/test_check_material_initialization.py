import unittest

from check_material_initialization import check


def counters(**overrides):
    result = {
        "materialFailures": 0,
        "materialSdkTranslationCalls": 1,
        "materialConversionCalls": 1,
        "materialGenerationCalls": 1,
        "materialFallbackTranslations": 0,
        "materialClassCompilationSeconds": 0.2,
        "materialNativeJits": 1,
        "materialNativeJitSeconds": 0.3,
        "materialProgramWaitSeconds": 0.05,
        "materialWorkerCpuSeconds": 0.4,
        "materialResourceReadBytesCumulative": 128,
        "processMaterialResourceReadCalls": 1,
        "processMaterialResourceHashCalls": 1,
        "processMaterialResourceBytesRead": 128,
        "processTextureHashCalls": 1,
        "processTextureSourceBytesHashed": 64,
        "processTextureSourceWaitSeconds": 0.01,
        "processTextureContentWaitSeconds": 0.02,
    }
    result.update(overrides)
    return result


def operation(name, stats, elapsed=1.0):
    return {"operation": name, "elapsedSeconds": elapsed,
            "processCpuSeconds": 0.8, "after": stats,
            "numericDifference": {}}


def run(operations, *, axis="identical_materials", value=64, repeat=1, exit_code=0):
    return {"axis": axis, "value": value, "repeat": repeat,
            "exitCode": exit_code,
            "probe": {"operations": operations}}


class CheckMaterialInitializationTests(unittest.TestCase):
    def test_missing_work_counter_is_phase_attributed(self):
        stats = counters()
        del stats["materialGenerationCalls"]
        result = check({"runs": [run([operation("cold", stats)])]})
        self.assertTrue(any(item["phase"] == "cold" and
                            item["metric"] == "materialGenerationCalls" and
                            item["reason"] == "missing counter"
                            for item in result["workCountFailures"]))

    def test_failed_run_is_separate_from_work_gate_failures(self):
        result = check({"runs": [run([], exit_code=7)]})
        self.assertEqual(result["failures"]["usdviewRuns"][0]["exitCode"], 7)
        self.assertEqual(result["workCountFailures"], [])

    def test_renderer_counters_reset_at_recreation_but_process_counters_do_not(self):
        phases = [
            operation("cold", counters()),
            operation("recreate", counters(materialSdkTranslationCalls=0,
                                            materialConversionCalls=0,
                                            materialGenerationCalls=0,
                                            materialResourceReadBytesCumulative=0,
                                            materialNativeJits=0)),
            operation("material-parameter", counters(materialConversionCalls=1,
                                                       materialGenerationCalls=1)),
        ]
        result = check({"runs": [run(phases)]})
        self.assertFalse(result["workCountFailures"])

        # A process-wide resource counter cannot reset when a renderer is
        # recreated, even though renderer-lifetime counters may reset there.
        process_reset = operation("recreate", counters(materialConversionCalls=0,
                                                         materialGenerationCalls=0,
                                                         materialSdkTranslationCalls=0,
                                                         materialResourceReadBytesCumulative=0,
                                                         processMaterialResourceReadCalls=0,
                                                         processMaterialResourceBytesRead=0))
        result = check({"runs": [run([operation("cold", counters()), process_reset])]})
        self.assertTrue(any(item["phase"] == "recreate" and
                            item["metric"] == "processMaterialResourceReadCalls"
                            for item in result["workCountFailures"]))

        renderer_reset = operation("material-parameter", counters(materialConversionCalls=0))
        result = check({"runs": [run([operation("cold", counters()), renderer_reset])]})
        self.assertTrue(any(item["phase"] == "material-parameter" and
                            item["metric"] == "materialConversionCalls" and
                            item["reason"] == "renderer-lifetime counter reset outside recreation"
                            for item in result["workCountFailures"]))

    def test_one_repeat_is_not_a_performance_pass_or_miss(self):
        result = check({"runs": [run([operation("cold", counters())])]})
        target = next(item for item in result["timings"]
                      if item["axis"] == "identical_materials")
        self.assertIsNone(target["targetMet"])
        self.assertEqual(target["status"], "insufficient-repeats")
        self.assertEqual(result["performanceMisses"], [])

    def test_phase_report_separates_worker_cpu_and_program_wait(self):
        result = check({"runs": [run([operation("cold", counters())])]})
        phase = result["phaseAttribution"][0]
        self.assertEqual(phase["workerCpuSeconds"], 0.4)
        self.assertEqual(phase["processCpuSeconds"], 0.8)
        self.assertEqual(phase["programWaitSeconds"], 0.05)
        self.assertEqual(phase["textureHashCalls"], 1)

    def test_structural_edits_keep_process_resource_work_stable(self):
        row = operation("structural-transform", counters())
        row["numericDifference"] = {
            "materialConversionCalls": 0,
            "materialGenerationCalls": 0,
            "materialSdkTranslationCalls": 0,
            "materialResourceReadBytesCumulative": 0,
            "processMaterialResourceReadCalls": 0,
            "processMaterialResourceHashCalls": 0,
            "processTextureHashCalls": 0,
        }
        good = check({"runs": [run([row])]})
        self.assertFalse(good["workCountFailures"])
        row["numericDifference"]["processMaterialResourceHashCalls"] = 1
        bad = check({"runs": [run([row])]})
        self.assertTrue(any(item["metric"] == "processMaterialResourceHashCalls"
                            and item["phase"] == "structural-transform"
                            for item in bad["workCountFailures"]))

    def test_32_unique_textures_requires_one_conversion_generation_and_no_fallback(self):
        good = check({"runs": [run([operation("cold", counters())],
                                   axis="textures_unique", value=32)]})
        self.assertFalse(good["workCountFailures"])
        bad = check({"runs": [run([operation("cold", counters(materialConversionCalls=2,
                                                               materialGenerationCalls=0))],
                                   axis="textures_unique", value=32)]})
        expected = {"materialConversionCalls", "materialGenerationCalls"}
        self.assertTrue(expected.issubset({item["metric"] for item in bad["workCountFailures"]}))


if __name__ == "__main__":
    unittest.main()
