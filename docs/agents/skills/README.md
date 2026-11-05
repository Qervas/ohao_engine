# Skills waiting to be installed

Agent sessions in this repo cannot write under `.claude/`, so a changed skill waits here
until Frank installs it. `tests/docs/check_docs.py` checks a skill from `.claude/skills/`
when it is installed there, and from here otherwise.

Waiting:
- `verify/SKILL.md`: leads with `python tools/check.py`, the one build-and-verify command
  (sub-project 4, step 2); points failing goldens at their heatmap and crop and wedged
  GPUs at `ohao doctor`, runs `serve_bench` and says the check reruns a noisy bench (step
  6), adds `rpc_tests` and `check_serve.py` (step 5), `log_tests`, the validation line
  format and the `noisy` bench verdict (step 4), `test_golden_diff.py`, `check_doctor.py` (step 3) and
  `check_tests.py` (step 2) to the CPU gates and
  `deferred_determinism_gpu_test` to the GPU gates (step 1); says the deferred goldens are
  exact (step 1); adds the live host's `check_live.py` (step 9) and the end-to-end MCP
  session `e2e_mcp.py` (step 11) to the GPU gates.
- `build/SKILL.md` (sub-project 4, step 2): new files need no re-configure
  (`CONFIGURE_DEPENDS`), every SPIR-V consumer builds the shaders, and `tools/check.py` is
  the shortest path; Linux (2026-11-05): Ninja Multi-Config with the pinned slangc, `-j16`,
  the GCC failure lines, Windows kept as the other platform.

Install (from the repo root), then remove the drafts:

```
cp docs/agents/skills/verify/SKILL.md .claude/skills/verify/SKILL.md
cp docs/agents/skills/build/SKILL.md .claude/skills/build/SKILL.md
git rm -r docs/agents/skills
```
