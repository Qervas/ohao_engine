"""Test 12: the generated scene JSON Schema works in a stock validator.

Dumps the fixture registry's schema with reflect_tests, then checks that every sample in
samples/valid passes and every sample in samples/invalid fails. Then (step 9) writes the
engine's schema with `ohao schema` and checks that every checked-in scene,
assets/scenes/*.scene.json, passes it. Test-only Python: needs the `jsonschema` package.
Exit 0 on success; the last line is a JSON summary.

    python tests/reflect/check_scene_schema.py [path/to/reflect_tests.exe]
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import jsonschema

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main() -> int:
    exe = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "Release" / ("reflect_tests.exe" if os.name == "nt" else "reflect_tests")
    failures = []
    passed = 0
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(exe), "--dump-schema", tmp], check=True, stdout=subprocess.DEVNULL)
        schema = json.loads((pathlib.Path(tmp) / "scene.schema.json").read_text(encoding="utf-8"))
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

    # The engine's schema against the checked-in scenes.
    ohao = exe.parent / ("ohao.exe" if exe.suffix == ".exe" else "ohao")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(ohao), "schema", "-o", tmp], check=True, stdout=subprocess.DEVNULL)
        engine = json.loads((pathlib.Path(tmp) / "scene.schema.json").read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(engine)
    engine_validator = jsonschema.Draft202012Validator(engine)
    scenes = sorted((ROOT / "assets" / "scenes").glob("*.scene.json"))
    if not scenes:
        failures.append("no checked-in scenes under assets/scenes")
    for scene in scenes:
        errors = list(engine_validator.iter_errors(json.loads(scene.read_text(encoding="utf-8"))))
        name = f"assets/scenes/{scene.name}"
        if errors:
            failures.append(f"{name}: {errors[0].message}")
            print(f"  FAIL  {name}: {errors[0].message}")
        else:
            passed += 1
            print(f"  PASS  {name} (engine schema)")

    print(json.dumps({"suite": "check_scene_schema", "passed": passed,
                      "failed": len(failures), "failures": failures}))
    return 0 if not failures and passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
