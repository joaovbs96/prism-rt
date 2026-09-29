# Material initialization measurements

These measurements cover the synthetic structural matrix on an Intel Core i7-14700K with four material workers. Every matrix row is the median of three fresh `testusdview` processes using a fresh, empty renderer cache. The operating-system file cache was not flushed and may have been warm. "Cold" is therefore renderer-cold, not a claim of cold storage. The tested plugin was `hdPrism.dll`, SHA-256 `3641f23e44ab8af958e8f559c099ac653de7d37f203e8ab3445d090d31698d20`.

The dedicated 65-equivalent-identical-material case (64 materials plus its controlled edit material) completed in **1.803 s** median (target 4 s); the dedicated 32-unique-texture case completed in **2.290 s** median (target 10 s). The full matrix independently measured these cases at 1.741 s and 2.208 s. All three checker reports have zero failed runs, material failures, work-count failures, or performance misses. The dedicated checker results are in `build/material-init-final-identical64/check.json` and `build/material-init-final-textures32/check.json`; the full matrix report is `build/material-init-final-matrix/check.json`.

The table shows elapsed wall seconds for each phase. "Material edit" is a single parameter edit on the controlled material; "structural add" is the controlled structural-add operation. Cold process CPU is reported separately from wall time; process CPU accumulates work across threads and can exceed elapsed time. The table's program count is the median native JIT count after cold initialization; SDK translation counts matched those counts in all rows.

| Axis | Value | Cold wall (s) | Cold process CPU (s) | Recreate (s) | Material edit (s) | Structural add (s) | Native JITs |
|---|---:|---:|---:|---:|---:|---:|---:|
| assignments | 16 | 1.570 | 2.172 | 0.064 | 0.047 | 0.049 | 1 |
| assignments | 128 | 1.683 | 2.297 | 0.180 | 0.048 | 0.148 | 1 |
| assignments | 1,024 | 2.513 | 4.250 | 1.101 | 0.049 | 0.910 | 1 |
| identical materials | 1 | 1.688 | 2.172 | 0.178 | 0.045 | 0.184 | 1 |
| identical materials | 16 | 1.756 | 2.469 | 0.196 | 0.048 | 0.147 | 1 |
| identical materials | 64 | 1.741 | 2.219 | 0.229 | 0.048 | 0.196 | 1 |
| color variants | 1 | 1.764 | 2.281 | 0.163 | 0.048 | 0.148 | 1 |
| color variants | 16 | 1.682 | 2.141 | 0.163 | 0.045 | 0.149 | 1 |
| color variants | 64 | 1.736 | 4.016 | 0.262 | 0.047 | 0.182 | 1 |
| structures | 1 | 1.717 | 3.625 | 0.163 | 0.048 | 0.165 | 1 |
| structures | 4 | 3.754 | 7.125 | 0.180 | 0.048 | 0.162 | 4 |
| structures | 16 | 9.138 | 25.547 | 0.247 | 0.047 | 0.198 | 16 |
| placements | 16 | 1.605 | 2.203 | 0.048 | 0.049 | 0.048 | 1 |
| placements | 128 | 1.538 | 2.109 | 0.046 | 0.047 | 0.049 | 1 |
| placements | 1,024 | 1.545 | 2.203 | 0.046 | 0.045 | 0.051 | 1 |
| partition | 1 | 1.573 | 2.328 | 0.047 | 0.050 | 0.049 | 1 |
| partition | 16 | 1.570 | 2.125 | 0.063 | 0.049 | 0.050 | 1 |
| partition | 128 | 1.673 | 2.406 | 0.147 | 0.046 | 0.131 | 1 |
| texture aliases | 1 | 1.682 | 2.422 | 0.080 | 0.048 | 0.065 | 1 |
| texture aliases | 8 | 1.677 | 2.422 | 0.097 | 0.046 | 0.083 | 1 |
| texture aliases | 32 | 1.808 | 2.812 | 0.179 | 0.048 | 0.132 | 1 |
| unique textures | 1 | 1.678 | 2.500 | 0.064 | 0.046 | 0.067 | 1 |
| unique textures | 8 | 1.825 | 2.703 | 0.112 | 0.046 | 0.081 | 1 |
| unique textures | 32 | 2.208 | 4.266 | 0.294 | 0.048 | 0.115 | 1 |

The primary scaling cost left in this matrix is authored graph diversity: the 16-structure case produced 16 native JIT programs and took 9.138 s cold, versus 1.717 s with one structure. This is an observed residual for this fixture, not a general cost bound. At 1,024 assignments, cold initialization stayed at 2.513 s, while recreation and structural-add medians rose to 1.101 s and 0.910 s, respectively. The distinct-texture fixture read and hashed about 25.95 MB across 33 resource calls; its 32-texture cold median was 2.208 s.

The earlier `build/structural-final/results.json` run, recorded with the same hardware and cache method, measured 9.642 s cold for the 64-identical-material case and 72.882 s for 32 unique textures. It used plugin SHA-256 `5d2dc459f6f3c36c784b17c848c276c1129f2c92fa44703de953ba75a1b48081`; the current run uses a different binary, as expected for a before/after implementation comparison.

These are procedural scenes with a 64x64 viewport and four material workers. The parity and interaction evidence below is reported separately; these timings do not predict real-asset loading, which remains a separate validation task.

## Additional completed validation

The integrated test run passed **25/25 CTests** (`build/material-init-final-ctest3.log`). Three separate Prism usdview interaction runs passed in `build/material-init-usdview-final-prism-r1` through `r3`. Their navigation p95 values were 31-32 ms. Each run observed texture preparation pending during a superseding edit, discarded the obsolete snapshot build (counter 0 to 1), and displayed the final requested generation. The final images matched the independently settled reference; both texture variants used the same two SDK program translations, and the final material-failure count was zero. The earlier diagnostic run using Storm is excluded because Storm does not support the fixture's PPM dome; the successful interaction runs used Prism.

The controlled 256-spp validation completed in both bilinear and filtered modes (`build/hydra-validation-bilinear` and `build/hydra-validation-filtered`). The custom-versus-builtin runtime comparison was exact: normalized RMSE 0 and relative mean-luminance error 0 (`build/material-init-custom-builtin-parity.log`). Builtin reference validation is recorded under `build/material-init-builtin-reference`.

Filtered export parity results are in `build/material-init-export-parity.log` and `build/material-init-export-parity256.log`. At 1,024 spp, InOneWeekend measured NRMSE 7.33413e-5 and mean luminance error 6.89582e-7. At 256 spp, QuadTest measured NRMSE 5.45297e-9 and mean luminance error 7.26708e-11. CornellBox missed the 64-spp threshold (NRMSE 0.0121191, mean luminance error 0.000697289); its exact 256-spp rerun passed at NRMSE 0.00582431 and mean luminance error 0.000291943. This difference is consistent with sampling variance in these runs; it does not establish the cause. The initial export command's PowerShell wrapper reported an error on the Cornell 64-spp miss, which is retained as a miss rather than counted as a pass.
