# Project skills

The agent skills for this repo: `build`, `verify` and `add-component`. Their home is
`.claude/skills/`, which `.gitignore` tracks (the rest of `.claude/` stays local).

They are kept here until they are installed, because the session that wrote them
(sub-project 3, step 10) could neither write into `.claude/` nor delete the stale
Godot/scons skills there. To install, from the repo root:

    rm -r .claude/skills
    git mv docs/agents/skills .claude/skills
    git rm --cached .claude/skills/README.md && rm .claude/skills/README.md

`rm -r` removes the stale skills (untracked), `git mv` moves these in, and this file goes.
Every command in them is run by `python tests/docs/check_docs.py`, which reads both
locations.
