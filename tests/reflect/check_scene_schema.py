"""Test 12 (partly): the generated scene JSON Schema works in a stock validator.

Dumps the fixture registry's schema with reflect_tests, then checks that every sample in
samples/valid passes and every sample in samples/invalid fails. Test-only Python: needs
the `jsonschema` package. Exit 0 on success; the last line is a JSON summary.

    python tests/reflect/check_scene_schema.py [path/to/reflect_tests.exe]
"""
import json
import pathlib
import subprocess
import sys
import tempfile

import jsonschema

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main() -> int:
    exe = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "Release" / "reflect_tests.exe"
    failures = []
    passed = 0
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(exe), "--dump-schema", tmp], check=True, stdout=subprocess.DEVNULL)
        schema = json.loads((pathlib.Path(tmp) / "ohscene.schema.json").read_text(encoding="utf-8"))
        json.loads((pathlib.Path(tmp) / "ohao_schema.json").read_text(encoding="utf-8"))  # parses

    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)

    for expect_valid, folder in ((True, "valid"), (False, "invalid")):
        for sample in sorted((HERE / "samples" / folder).glob("*.json")):
            doc = json.loads(sample.read_text(encoding="utf-8"))
            errors = list(validator.iter_errors(doc))
            ok = (not errors) if expect_valid else bool(errors)
            name = f"{folder}/{sample.name}"
            if ok:
                passed += 1
                print(f"  PASS  {name}")
            else:
                detail = errors[0].message if errors else "accepted, expected rejection"
                failures.append(f"{name}: {detail}")
                print(f"  FAIL  {name}: {detail}")

    print(json.dumps({"suite": "check_scene_schema", "passed": passed,
                      "failed": len(failures), "failures": failures}))
    return 0 if not failures and passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
