# Skills waiting to be installed

Agent sessions in this repo cannot write under `.claude/`, so a changed skill waits here
until Frank installs it. `tests/docs/check_docs.py` checks a skill from `.claude/skills/`
when it is installed there, and from here otherwise.

Waiting:
- `verify/SKILL.md`: leads with `python tools/check.py`, the one build-and-verify command
  (sub-project 4, step 2); adds `check_tests.py` to the CPU gates (step 2) and
  `deferred_determinism_gpu_test` to the GPU gates (step 1); says the deferred goldens are
  exact (step 1).
- `build/SKILL.md` (sub-project 4, step 2): new files need no re-configure
  (`CONFIGURE_DEPENDS`), every SPIR-V consumer builds the shaders, and `tools/check.py` is
  the shortest path.

Install (from the repo root), then remove the drafts:

```
cp docs/agents/skills/verify/SKILL.md .claude/skills/verify/SKILL.md
cp docs/agents/skills/build/SKILL.md .claude/skills/build/SKILL.md
git rm -r docs/agents/skills
```
