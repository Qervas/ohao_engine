# Skills waiting to be installed

Agent sessions in this repo cannot write under `.claude/`, so a changed skill waits here
until Frank installs it. `tests/docs/check_docs.py` checks a skill from `.claude/skills/`
when it is installed there, and from here otherwise.

Waiting:
- `verify/SKILL.md` (sub-project 4, step 1): adds `deferred_determinism_gpu_test` to the
  GPU gates, and says the deferred goldens are exact.

Install (from the repo root), then remove the draft:

```
cp docs/agents/skills/verify/SKILL.md .claude/skills/verify/SKILL.md
git rm -r docs/agents/skills
```
