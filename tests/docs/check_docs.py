"""Runs every command the agent-facing docs show, and checks what they say it prints.

The docs (AGENTS.md, docs/ARCHITECTURE.md, docs/agents/ohao-cli.md, docs/agents/ohao-serve.md and the skills under
.claude/skills/ or docs/agents/skills/) write runnable commands in ```console blocks:

    $ build/Release/ohao validate assets/scenes/cornell_box.scene.json
    {"result":{"ok":true}}

- A `$ ` line is a command, run with Git Bash from the repo root. The lines after it, up
  to the next `$ ` line or the end of the block, are its expected stdout, compared
  exactly (trailing spaces ignored). A line holding only `...` matches any run of lines.
  No lines after it: only the exit code is checked.
- A trailing `# exit N` expects exit code N (default 0). `# slow` marks a command run
  only with --slow (full builds, the golden corpus, the bench gate); `# gpu` one skipped
  with --cpu-only. Markers combine: `# exit 1 # gpu`.
- Each file starts from an empty build/doc_tmp; its commands run in order, so a later
  command may read what an earlier one wrote there.

Other fences (```bash, ```cpp, ...) are prose, never run.

    python tests/docs/check_docs.py [--slow] [--cpu-only] [FILE ...]

The last line printed is a JSON summary: {"suite": "doc_commands", "passed", "failed", "failures"}.
"""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TMP = os.path.join(ROOT, "build", "doc_tmp")
DEFAULT_FILES = ["AGENTS.md", "docs/ARCHITECTURE.md", "docs/agents/ohao-cli.md", "docs/agents/ohao-serve.md"]
MARKER = re.compile(r"\s+#\s*(exit\s+\d+|slow|gpu)\s*$")


def bash():
    # Git Bash, never WSL's System32\bash.exe (a different filesystem view).
    for cand in (r"C:\Program Files\Git\bin\bash.exe", shutil.which("bash") or ""):
        if cand and os.path.isfile(cand) and "system32" not in cand.lower():
            return cand
    raise SystemExit("check_docs: Git Bash not found")


def skill_files():
    # The installed skills, and the ones waiting in docs/agents/skills to be installed
    # (see its README). A skill present in both is checked once, from .claude/skills.
    found = {}
    for base in (os.path.join(ROOT, "docs", "agents", "skills"), os.path.join(ROOT, ".claude", "skills")):
        if os.path.isdir(base):
            for d in sorted(os.listdir(base)):
                if os.path.isfile(os.path.join(base, d, "SKILL.md")):
                    found[d] = os.path.relpath(os.path.join(base, d, "SKILL.md"), ROOT).replace("\\", "/")
    return [found[k] for k in sorted(found)]


def parse(path):
    """[(line_no, command, exit_code, flags, expected_lines or [])] for every console block."""
    out, in_block, current = [], False, None
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        lines = f.read().splitlines()
    for no, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("```"):
            if in_block:
                in_block, current = False, None
            elif stripped == "```console":
                in_block = True
            continue
        if not in_block:
            continue
        if stripped.startswith("$ "):
            cmd, code, flags = stripped[2:], 0, set()
            while True:
                m = MARKER.search(cmd)
                if not m:
                    break
                tag = m.group(1)
                if tag.startswith("exit"):
                    code = int(tag.split()[1])
                else:
                    flags.add(tag)
                cmd = cmd[:m.start()]
            current = [no, cmd.strip(), code, flags, []]
            out.append(current)
        elif current is not None:
            current[4].append(line.rstrip())
    for c in out:   # drop trailing blank expected lines
        while c[4] and not c[4][-1]:
            c[4].pop()
    return out


def matches(expected, got):
    """Exact line match where a lone `...` line matches any run of lines (including none)."""
    if not expected:
        return not got
    if expected[0] == "...":
        return any(matches(expected[1:], got[i:]) for i in range(len(got) + 1))
    return bool(got) and expected[0] == got[0] and matches(expected[1:], got[1:])


def main(argv):
    slow, cpu_only = "--slow" in argv, "--cpu-only" in argv
    files = [a for a in argv if not a.startswith("--")] or DEFAULT_FILES + skill_files()
    sh = bash()
    passed, failures, skipped = 0, [], 0
    for path in files:
        shutil.rmtree(TMP, ignore_errors=True)
        os.makedirs(TMP, exist_ok=True)
        print(f"{path}")
        for no, cmd, code, flags, expected in parse(path):
            if ("slow" in flags and not slow) or ("gpu" in flags and cpu_only):
                skipped += 1
                print(f"  [SKIP]  {path}:{no}  {cmd}")
                continue
            if os.name != "nt":   # the docs name Windows binaries; elsewhere the same binary has no suffix
                cmd = re.sub(r"(build/Release/[\w-]+)\.exe\b", r"\1", cmd)
            p = subprocess.run([sh, "-c", cmd], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=3600)
            got = [l.rstrip() for l in p.stdout.decode("utf-8", "replace").replace("\r\n", "\n").splitlines()]
            while got and not got[-1]:
                got.pop()
            problems = []
            if p.returncode != code:
                tail = p.stderr.decode("utf-8", "replace").strip().splitlines()[-3:]
                problems.append(f"exit {p.returncode}, expected {code}; stderr tail {tail}")
            if expected and not matches(expected, got):
                problems.append("stdout differs:\n" + "\n".join(f"        got:  {l}" for l in got[:12]) + "\n" +
                                "\n".join(f"        want: {l}" for l in expected[:12]))
            if problems:
                failures.append(f"{path}:{no}")
                print(f"  [FAIL]  {path}:{no}  {cmd}")
                for pr in problems:
                    print(f"      {pr}")
            else:
                passed += 1
                print(f"  [PASS]  {path}:{no}  {cmd}")
    print(f"Results: {passed}/{passed + len(failures)} passed, {skipped} skipped")
    print(json.dumps({"suite": "doc_commands", "passed": passed, "failed": len(failures), "failures": failures}))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
