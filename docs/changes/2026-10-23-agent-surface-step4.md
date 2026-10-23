# Sub-project 4, step 4: the engine logger

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Logging" and its as-built note; "Performance / Budgets", the two OHAO_LOG rows; "Build order" step 4; "Test plan" tests 6 and 2)
**Status:** Done.

## 1. Summary

Every engine message now goes through one logger with a level and a channel, kept in a ring an agent can page later (log.read in step 5), and never written to stdout, which `ohao serve` will own for its protocol.

## 2. What was built

- `ohao/core/log.hpp`, `ohao/core/log.cpp`. Macro `OHAO_LOG(level, channel, fmt, args...)`: level tokens Trace, Debug, Info, Warn, Error; channels render, gpu, validation, ecs, asset, physics, audio, api, rpc, runtime; the rest is a std::format string checked at compile time. Below the capture threshold: one relaxed atomic load and a compare, arguments not evaluated. Above it: formatted into a thread-local buffer, cut at 1 KB on a whole UTF-8 character, written to the ring under one mutex.
- Ring: newest 4,096 records, each {seq, t (seconds since start), frame, level, channel, repeat, msg}. Identical consecutive records (same level, channel, text) collapse into one with a repeat count (no new seq). `read(since, minLevel, channel, limit)` pages by seq and reports `dropped` (records after `since` that fell off). `stats()`, `clear()`.
- Sinks: own level each; write() and repeated() called under the logger's lock in record order; a sink must not log. Default stderr sink prints `<L> <channel>: <message>` (L is one of T D I W E) and folds a run of repeats into a line `<L> <channel>: ... repeated N more times`. `OHAO_LOG_LEVEL=trace|debug|info|warn|error|off` sets the threshold at start-up. The logger's state is a leaked singleton so records written during static teardown are safe.
- The renderer stamps the frame (`log::setFrame`) as each deferred or RT frame starts (`VulkanRenderer::m_framesStarted`).
- All 425 print calls under ohao/ (56 files) migrated to OHAO_LOG with a channel and a level; per-item and per-frame detail is Debug, so a default deferred Cornell render now prints 15 engine lines to stderr. The old `ConsoleWidget` facade (ohao/core/console_widget.{hpp,cpp}, used by five physics files with its own one-argument OHAO_LOG(msg) macro) is deleted.
- Vulkan validation messages are records on the `validation` channel (Error or Warn by the layer's severity) as `<VUID> | <text>`, stamped with the frame. `tests/validation/check_validation.py` reads the new lines and unfolds the repeat lines.
- Test 2 (print part): `reflect_tests` `sp4_t02_lint_finds_prints` and `sp4_t02_engine_prints_only_through_the_logger`: no std::cout, std::cerr, std::clog, printf, fprintf, puts, fputs, putchar, stdout or stderr under ohao/ as whole words in code (comments skipped; snprintf is not printf); one allowlist entry, the logger's own stderr sink in ohao/core/log.cpp.
- Test 6: `tests/log/log_tests.cpp`, suite `log_tests` (label quick), 10 tests `sp4_t06_*`: level filtering (and arguments below the threshold are not evaluated), ring wrap and dropped, repeat collapse, channel filter, since paging, concurrent writers (8 threads x 500 records, no torn record, each thread's order kept), sink routing, frame/time stamps and the 1 KB UTF-8 cut, level and channel names, the stderr line format with repeats folded (stderr redirected to a file).
- `log_bench` (tools/bench/log_bench.cpp, label bench, run alone) gates the two log budgets from `tests/bench/serve_budgets.json` (new file; step 6 adds the rest): log_below_threshold <= 5 ns per call, log_ring_80 <= 300 ns per call (80-character message into the ring, no sinks). Medians of 50 blocks after 5 warm-ups, p95 reported, high priority. `tools/bench/bench_env.hpp` (shared with step 6's serve_bench): a calibration loop timed before and after the blocks, compared with `build/serve_bench_reference.json` written by `log_bench --calibrate` (23.184 ms on this machine); more than 25% slower is `noisy`. At the reviewer's request it also records every other process that used CPU during the run; more than 50% of a core in total is also `noisy`. A noisy run exits 2 and is neither a pass nor a failure. Results in build/log_bench.json.
- Docs: AGENTS.md (log_tests, log_bench, the validation line format, the noisy verdict, the logging convention), docs/ARCHITECTURE.md (ohao_core owns the logger; the dependency rule "the engine prints only through the logger"), the verify skill draft in docs/agents/skills/, the spec's as-built notes, the monograph: the "Console widget" unit is replaced by a "Logger" unit (core/log), and three units that cited migrated print lines (core/event-bus, hybrid/gi-technique, path-tracer/bindings) plus reflection/cli were updated; site rebuilt (92 leaf pages, 17 modules).

## 3. Bugs found and fixed

1. `denoise_parse_test` compiles `denoise_types.cpp` directly and did not link ohao_core, so it failed to link once that file logged; it now links ohao_core.
2. `engine_tests` tested the deleted `LogLevel` enum; that test now checks `log::Level` (same test count, 58).
3. The OIDN error path would have formatted a null `const char*` (std::format does not accept null); it now logs "(no message)". That code is compiled only with OIDN, which this machine's build does not have.

## 4. Spec corrections

The spec says serve_bench measures the log budgets; serve_bench does not exist until step 6, so log_bench measures them now and step 6 folds them in (the spec's build-order note says so). The check script's rerun of a noisy bench stays step 6's.

## 5. Results

- log_tests 10/10; reflect_tests 35/35 (including the two new print-lint tests); engine_tests 58/58; check_validation 2/2 clean (deferred Cornell and the helmet); the validation-line parser was also checked on sample output with a repeat line.
- `python tools/check.py --all` (build 12.5 s, doctor ok with the tools.cmake warning): 33/34 passed, 34 tests registered (step 3: 32; new: log_tests, log_bench). Counts from the suites: check_cli_cpu 25, check_scene_schema 16, check_tests 7, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_cli 29, check_validation 2, render_golden 10, check_docs 56, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43; ohao_game_presentation, monograph_structure and reflect_exhaustive_floats (339.6 s) passed. scene_bench failed (see Benchmark).
- Render goldens 10/10 (the deferred four exact).

### Benchmark

- log_bench: six passing runs and no failing run; ring medians 200.41, 194.61, 199.22, 203.42, 204.31 ns plus the CTest run (passed), p95 at most 211.33 ns; below the threshold 0.20 ns median every run. Three further runs were noisy (other processes at 62.3%, 85.5% and 53.0% of a core; their ring medians 200.65, 204.25, 205.76 ns). Every run recorded UnrealEditor.exe (another bot's process) using 12-37% of a core.
- scene_bench (this step changes none of its code paths' costs) failed again as in steps 2 and 3, with the same shape: its cold bench10k block measured save_pretty 8.993 ms, load 13.050 ms, validate 8.052 ms (step 3: 8.900, 12.837, 8.204), and the warm rounds that follow measured 21.384, 30.496, 19.982 ms. A log_bench run right after recorded UnrealEditor.exe at 24.9% of a core. Per the reviewer's ruling this is external-load contention, recorded, not a regression; no budget was changed.

## 6. Known gaps

- scene_bench cannot be judged while another bot's UnrealEditor runs on the machine; it has no load record of its own yet (step 6's serve_bench uses bench_env.hpp).
- log.read over the protocol and the RPC sink arrive with `ohao serve` in step 5.
- The verify skill change waits in docs/agents/skills/ until Frank installs it.

## 7. Running the tests

```
build/Release/log_tests.exe
build/Release/reflect_tests.exe
build/Release/log_bench.exe --check tests/bench/serve_budgets.json
build/Release/log_bench.exe --calibrate
python tests/validation/check_validation.py
OHAO_LOG_LEVEL=debug build/Release/cornell_box.exe build/out.png 4 deferred --denoise=none
python tools/check.py --all
```
