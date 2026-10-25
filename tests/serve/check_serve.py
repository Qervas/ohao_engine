#!/usr/bin/env python3
"""
check_serve -- `ohao serve` as a process, end to end (sub-project 4, step 5).

  test 8   MCP: every tool's inputSchema is a valid draft 2020-12 schema (jsonschema),
           accepts the example arguments below and refuses an unknown member, exactly as
           the server does; tools/list is at most 24 KB.
  test 9   protocol goldens: each scripted session in tests/serve/sessions/*.jsonl runs
           against `ohao serve` (headless, CPU) twice; stdout must equal
           tests/serve/expected/<name>.jsonl byte for byte both times.
  test 10  stdout stays clean: rpc_print_server's test.print writes to stdout through every
           C and C++ path; every stdout line must still be a protocol message, and the
           prints must have reached stderr.
  test 15  absolute paths, '..' and writes outside --out are E_ACCESS; --allow-path admits
           exactly its root; the message-size, step, batch, input and page caps hold;
           `--pipe NAME` (Windows) serves one local client: a second client, a client
           through \\localhost and a second server on the name are refused, nothing reaches
           stdout, and the client's disconnect ends the session with exit 0.

Usage (from the repo root):
  python tests/serve/check_serve.py            # all
  python tests/serve/check_serve.py --update   # rewrite the goldens (read the diff first)

The last stdout line is the shared JSON summary {"suite", "passed", "failed", "failures"}.
Scratch output goes under build/serve_tmp/ (per process). Needs `jsonschema`.
"""
import json
import os
import queue
import shutil
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = ".exe" if os.name == "nt" else ""
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + EXE)
PRINT_SERVER = os.path.join(ROOT, "build", "Release", "rpc_print_server" + EXE)
SESSIONS = os.path.join(ROOT, "tests", "serve", "sessions")
EXPECTED = os.path.join(ROOT, "tests", "serve", "expected")
TMP = os.path.join(ROOT, "build", "serve_tmp", str(os.getpid()))
NL = chr(10)

# One accepted example per tool (the agent guide's examples arrive with it in step 11).
EXAMPLES = {
    "engine_info": {},
    "schema_get": {},
    "scene_validate": {"path": "tests/scenes/v1/rooms.scene.json", "limit": 5},
    "scene_load": {"path": "tests/scenes/v1/rooms.scene.json", "seed": "00000000000000aa", "replace": True},
    "scene_save": {"path": "OUT/example.scene.json"},
    "world_info": {},
    "world_summary": {"depth": 1, "maxChildren": 4},
    "world_query": {"names": ["Lamp"], "fields": ["/Light/intensity"], "limit": 5},
    "world_apply": {"commands": [{"op": "entity.create", "name": "Box"}]},
    "world_step": {"frames": 2, "watch": {"names": ["Lamp"], "fields": ["/Transform/position"]}},
    "time_get": {},
    "time_set": {"fixedDt": 0.02, "paused": False},
    "world_hash": {},
    "input_inject": {"events": [{"step": 0, "axis": "move_forward", "value": 1}, {"step": 2, "key": "Space", "down": True},
                                {"step": 3, "mouse": [4, -2]}, {"step": 4, "focusLost": True}]},
    "input_state": {},
    "input_record": {"path": "OUT/example.input.jsonl"},
    "input_replay": {"path": "OUT/example.input.jsonl", "verify": False},
    "world_snapshot": {"name": "a"},
    "world_diff": {"snapshot": "a", "fields": ["/Transform"], "limit": 5},
    "world_restore": {"name": "a"},
    "render_capture": {"path": "OUT/x.png", "mode": "deferred", "width": 64, "height": 64, "post": ["taa"]},
    "render_stats": {"frames": 30, "passes": True},
    "render_settings": {"set": {"rtGI": {"rays": 4}, "ssr": False}},
    "log_read": {"level": "warn", "channel": "rpc", "limit": 10},
}
# render_capture would start a GPU; render_stats before any capture is E_STATE (checked on
# its own); input_replay would read the recording input_record is still writing (api_tests
# replays recordings).
CALL = [n for n in EXAMPLES if n not in ("render_capture", "render_stats", "input_replay")]

passed = 0
failed = 0
failures = []


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
        print("  ok    " + what)
    else:
        failed += 1
        failures.append(what)
        print("  FAIL  " + what)


class Server:
    """A server process: lines in on stdin, lines out on stdout (stderr to a file)."""

    def __init__(self, argv, name):
        os.makedirs(TMP, exist_ok=True)
        self.err_path = os.path.join(TMP, name + ".stderr.txt")
        self.err = open(self.err_path, "wb")
        self.proc = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err)
        self.lines = queue.Queue()
        self.raw = []
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.raw.append(line)
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        if isinstance(line, dict):
            line = json.dumps(line, separators=(",", ":"))
        data = line.encode("utf-8") if isinstance(line, str) else line
        if not data.endswith(b"\n"):
            data += b"\n"
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def wait_reply(self, rid, timeout=120):
        """The reply with this id (parsed); stdout lines before it are kept in self.raw."""
        while True:
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty:
                return None
            if line is None:
                return None
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if isinstance(msg, dict) and "method" not in msg and msg.get("id") == rid:
                return msg

    def call(self, rid, method, params=None):
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return self.wait_reply(rid)

    def close(self, timeout=60):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = "timeout"
        self.err.close()
        return code

    def stderr(self):
        with open(self.err_path, "rb") as f:
            return f.read().decode("utf-8", "replace")


def serve(name, *extra):
    out = os.path.relpath(os.path.join(TMP, name), ROOT).replace(os.sep, "/")
    return Server([OHAO, "serve", "--out", out, *extra], name), out


# ---- test 9: protocol goldens --------------------------------------------------------------

def run_session(name):
    """stdout bytes of one scripted session, or None if it did not finish."""
    with open(os.path.join(SESSIONS, name + ".jsonl"), "rb") as f:
        lines = [l.rstrip(b"\r\n") for l in f.read().split(b"\n") if l.strip()]
    s = Server([OHAO, "serve", "--out", "build/serve_tmp/" + name], name)
    for l in lines:
        s.send(l)
    done = s.wait_reply("end") is not None
    code = s.close()
    return (b"".join(s.raw) if done and code == 0 else None), code


def test_goldens(update):
    print("test 9: protocol goldens")
    for file in sorted(os.listdir(SESSIONS)):
        name = file[:-len(".jsonl")]
        first, code1 = run_session(name)
        second, code2 = run_session(name)
        check(first is not None and second is not None, name + ": the session answers 'end' and exits 0 (exits "
              + str(code1) + ", " + str(code2) + ")")
        if first is None or second is None:
            continue
        check(first == second, name + ": byte-identical twice")
        golden = os.path.join(EXPECTED, name + ".jsonl")
        if update:
            os.makedirs(EXPECTED, exist_ok=True)
            with open(golden, "wb") as f:
                f.write(first)
            print("        updated " + os.path.relpath(golden, ROOT))
            continue
        want = open(golden, "rb").read() if os.path.exists(golden) else b""
        check(first == want, name + ": stdout equals " + os.path.relpath(golden, ROOT).replace(os.sep, "/"))
        if first != want:
            for a, b in zip(first.split(b"\n"), want.split(b"\n")):
                if a != b:
                    print("        got:  " + a.decode("utf-8", "replace")[:300])
                    print("        want: " + b.decode("utf-8", "replace")[:300])
                    break
        for line in first.split(b"\n")[:-1]:
            try:
                json.loads(line)
            except ValueError:
                check(False, name + ": every stdout line is JSON")
                break


# ---- test 8: MCP schemas through jsonschema ----------------------------------------------------

def test_schemas():
    print("test 8: MCP tool schemas")
    try:
        import jsonschema
    except ImportError:
        check(False, "jsonschema is installed (pip install jsonschema)")
        return
    s, out = serve("schemas")
    init = s.call(1, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "t"}})
    check(init is not None and init["result"]["protocolVersion"] == "2025-11-25", "initialize")
    s.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    listed = s.call(2, "tools/list")
    tools = {t["name"]: t for t in listed["result"]["tools"]}
    size = len(json.dumps(listed["result"], separators=(",", ":")).encode("utf-8"))
    print("        tools/list: " + str(size) + " bytes, " + str(len(tools)) + " tools")
    check(size <= 24 * 1024, "tools/list is at most 24 KB")
    check(sorted(tools) == sorted(EXAMPLES), "one tool per method, '.' -> '_' (" + ", ".join(sorted(tools)) + ")")
    rid = 10
    for name in EXAMPLES:   # in this order: world_snapshot before world_restore
        if name not in tools:
            continue
        schema = tools[name]["inputSchema"]
        try:
            jsonschema.Draft202012Validator.check_schema(schema)
            valid = True
        except jsonschema.SchemaError as e:
            valid = False
            print("        " + name + ": " + str(e.message))
        check(valid, name + ": inputSchema is a valid draft 2020-12 schema")
        v = jsonschema.Draft202012Validator(schema)
        example = json.loads(json.dumps(EXAMPLES.get(name, {})).replace("OUT", out))
        errors = [e.message for e in v.iter_errors(example)]
        check(not errors, name + ": accepts its example " + json.dumps(example) + (" " + str(errors) if errors else ""))
        check(not v.is_valid(dict(example, notAMember=1)), name + ": refuses an unknown member")
        if name in CALL:
            rid += 1
            r = s.call(rid, "tools/call", {"name": name, "arguments": example})
            ok = r is not None and "result" in r and not r["result"].get("isError")
            check(ok, name + ": the server accepts the example too" + ("" if ok else " " + json.dumps(r)[:300]))
            rid += 1
            r = s.call(rid, "tools/call", {"name": name, "arguments": dict(example, notAMember=1)})
            check(r is not None and r.get("error", {}).get("code") == -32602, name + ": and refuses the unknown member")
    rid += 1
    r = s.call(rid, "tools/call", {"name": "render_stats", "arguments": {}})
    check(r is not None and r.get("result", {}).get("isError") and "E_STATE" in json.dumps(r),
          "render_stats before any frame is rendered: E_STATE (and no GPU is started)")
    check(s.close() == 0, "exits 0 when stdin closes")


# ---- test 10: stdout stays clean ------------------------------------------------------------

def test_stdout_clean():
    print("test 10: stdout carries protocol messages only")
    s = Server([PRINT_SERVER], "print")
    r1 = s.call(1, "test.print", {})
    r2 = s.call(2, "world.info")
    code = s.close()
    check(r1 is not None and r1.get("result") == {"printed": True}, "test.print answers")
    check(r2 is not None and "result" in r2, "and the next request too")
    bad = []
    for line in s.raw:
        try:
            msg = json.loads(line)
            if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
                bad.append(line)
        except ValueError:
            bad.append(line)
    check(not bad, "every stdout line is a JSON-RPC message" + ("" if not bad else " (" + repr(bad[:3]) + ")"))
    err = s.stderr()
    for what in ["printf", "puts", "fprintf", "fwrite", "fputs", "cout", "cerr"] + (
            ["_write", "WriteFile"] if os.name == "nt" else ["write"]):
        check("STRAY " + what in err, "the " + what + " print reached stderr")
    check(code == 0, "exits 0")


# ---- test 15 (paths part) and the caps ------------------------------------------------------

def test_sandbox():
    print("test 15: the sandbox and the caps")
    allow_a = os.path.join(TMP, "allowA")
    allow_ab = os.path.join(TMP, "allowAB")
    os.makedirs(allow_a, exist_ok=True)
    os.makedirs(allow_ab, exist_ok=True)
    shutil.copy(os.path.join(ROOT, "tests", "scenes", "v1", "rooms.scene.json"), allow_ab)
    s, out = serve("sandbox", "--allow-path", os.path.relpath(allow_a, ROOT), "--max-message", "2048")

    def code_of(rid, method, params):
        r = s.call(rid, method, params)
        if r is None:
            return "no reply"
        if "result" in r:
            return "ok"
        data = r["error"].get("data")
        return data.get("code") if isinstance(data, dict) else r["error"]["code"]

    check(code_of(1, "scene.load", {"path": "tests/scenes/v1/rooms.scene.json"}) == "ok", "a project-relative read")
    check(code_of(2, "scene.validate", {"path": os.path.join(ROOT, "tests", "scenes", "v1", "rooms.scene.json")})
          == "E_ACCESS", "an absolute path outside the allowed roots: E_ACCESS")
    check(code_of(3, "scene.validate", {"path": "tests/scenes/../scenes/v1/rooms.scene.json"}) == "E_ACCESS",
          "'..': E_ACCESS")
    check(code_of(4, "scene.save", {"path": "tests/x.scene.json"}) == "E_ACCESS", "a write outside --out: E_ACCESS")
    check(code_of(5, "scene.save", {"path": out + "/deep/x.scene.json"}) == "ok" and
          os.path.exists(os.path.join(ROOT, out, "deep", "x.scene.json")), "a write under --out")
    check(code_of(6, "scene.save", {"path": os.path.join(allow_a, "y.scene.json").replace(os.sep, "/")}) == "ok",
          "an absolute write inside --allow-path")
    check(code_of(7, "scene.validate", {"path": os.path.join(allow_ab, "rooms.scene.json").replace(os.sep, "/")})
          == "E_ACCESS", "--allow-path admits exactly its root (not a sibling sharing its prefix)")
    s.send('{"jsonrpc":"2.0","id":8,"method":"world.info","params":{"pad":"' + "x" * 3000 + '"}}')
    r = s.wait_reply(None)
    check(r is not None and r["error"]["code"] == -32600, "a message over --max-message: -32600")
    check(code_of(9, "world.info", {}) == "ok", "and the server reads on")
    check(code_of(10, "world.step", {"frames": 1000001}) == -32602, "the step cap: -32602")
    check(s.close() == 0, "exits 0")


def test_caps():
    print("test 15: the batch, input and page caps")
    s, _ = serve("caps")

    def error_of(rid, method, params):
        r = s.call(rid, method, params)
        if r is None:
            return "no reply"
        if "result" in r:
            return "ok"
        data = r["error"].get("data")
        first = data[0]["code"] if isinstance(data, list) and data else (data or {}).get("code")
        return str(r["error"]["code"]) + " " + str(first)

    create = {"op": "entity.create"}
    check(error_of(1, "world.apply", {"commands": [create] * 10001}) == "-32602 E_RANGE",
          "world.apply with 10,001 commands: -32602 E_RANGE")
    check(error_of(2, "world.apply", {"commands": [create] * 10000}) == "ok", "10,000 apply")
    mouse = {"mouse": [1, 0]}
    check(error_of(3, "input.inject", {"events": [mouse] * 10001}) == "-32602 E_RANGE",
          "input.inject with 10,001 events: -32602 E_RANGE")
    for i in range(10):
        s.call(10 + i, "input.inject", {"events": [mouse] * 10000})
    check(error_of(20, "input.inject", {"events": [mouse]}) == "1 E_STATE", "over 100,000 waiting events: E_STATE")
    check(error_of(21, "log.read", {"limit": 1001}) == "-32602 E_RANGE", "a log page over 1,000 lines: -32602 E_RANGE")
    check(error_of(22, "world.query", {"limit": 100001}) == "-32602 E_RANGE", "a query page over 100,000: -32602 E_RANGE")
    check(s.close() == 0, "exits 0")


# ---- test 15 (rest): the named pipe ---------------------------------------------------------

def test_pipe():
    print("test 15: the named pipe")
    import ctypes
    import time
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    k32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]

    def refusal(path):
        """The Windows error a client opening `path` gets, or 0 if it was admitted (then closed)."""
        h = k32.CreateFileW(path, 0xC0000000, 0, None, 3, 0, None)
        if h in (None, wintypes.HANDLE(-1).value):
            return ctypes.get_last_error()
        k32.CloseHandle(h)
        return 0

    name = "check-serve-%d" % os.getpid()
    path = "\\\\.\\pipe\\ohao-" + name
    # stdin stays open and unused: the pipe's end, not stdin's, ends the session.
    s, out = serve("pipe", "--pipe", name)
    up = False
    for _ in range(500):
        if k32.WaitNamedPipeW(path, 20):
            up = True
            break
        time.sleep(0.02)
    check(up, "the server creates " + path)
    check(refusal("\\\\localhost\\pipe\\ohao-" + name) == 5,
          "a client through \\\\localhost (a remote client to the pipe) is refused: error 5")
    second_server = subprocess.run([OHAO, "serve", "--pipe", name], cwd=ROOT, stdin=subprocess.DEVNULL,
                                   capture_output=True, timeout=60)
    check(second_server.returncode == 1 and b"already exists" in second_server.stdout,
          "a second server on the same name exits 1: already exists")
    f = None
    for _ in range(250):
        try:
            f = open(path, "r+b", buffering=0)
            break
        except OSError:
            time.sleep(0.02)
    check(f is not None, "a local client connects")
    if f is None:
        s.close()
        return
    check(refusal(path) == 231, "a second client is refused: ERROR_PIPE_BUSY (231)")
    pending = b""

    def call(rid, method, params=None):
        nonlocal pending
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        f.write(json.dumps(msg, separators=(",", ":")).encode() + b"\n")
        while True:
            while b"\n" not in pending:
                chunk = f.read(65536)
                if not chunk:
                    return None
                pending += chunk
            line, pending = pending.split(b"\n", 1)
            reply = json.loads(line)
            if "method" not in reply and reply.get("id") == rid:
                return reply

    init = call(1, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "t"}})
    check(init is not None and init["result"]["serverInfo"]["name"] == "ohao", "MCP initialize over the pipe")
    load = call(2, "scene.load", {"path": "tests/scenes/v1/rooms.scene.json"})
    step = call(3, "world.step", {"frames": 3})
    check(load is not None and "result" in load and step is not None and step["result"]["frame"] == 3,
          "native methods over the pipe (load, step)")
    saved = call(4, "scene.save", {"path": out + "/via_pipe.scene.json"})
    check(saved is not None and "result" in saved and os.path.exists(os.path.join(ROOT, out, "via_pipe.scene.json")),
          "the sandbox applies over the pipe as over stdio")
    f.close()
    try:
        ended = s.proc.wait(timeout=30)   # stdin is still open: only the pipe's end can end it
    except subprocess.TimeoutExpired:
        ended = "timeout"
    check(ended == 0 and s.close() == 0, "the client's disconnect ends the session, stdin still open: exit 0")
    check(not s.raw, "nothing on stdout: protocol messages go to the pipe")
    check("serving on " + path in s.stderr(), "stderr names the pipe")
    bad = subprocess.run([OHAO, "serve", "--pipe", "a/b"], cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True,
                         timeout=60)
    check(bad.returncode == 1 and b"E_COMMAND" in bad.stdout, "an invalid pipe name: exit 1, E_COMMAND")


def main():
    update = "--update" in sys.argv[1:]
    for exe in (OHAO, PRINT_SERVER):
        if not os.path.exists(exe):
            print("missing " + exe + "; build first (C:/Users/djmax/scoop/shims/cmake --build build --config Release)")
            print(json.dumps({"suite": "check_serve", "passed": 0, "failed": 1, "failures": ["not built"]}))
            return 2
    shutil.rmtree(TMP, ignore_errors=True)
    test_goldens(update)
    if not update:
        test_schemas()
        test_stdout_clean()
        test_sandbox()
        test_caps()
        if os.name == "nt":
            test_pipe()
    shutil.rmtree(TMP, ignore_errors=True)
    shutil.rmtree(os.path.join(ROOT, "build", "serve_tmp"), ignore_errors=True)
    print("Results: " + str(passed) + "/" + str(passed + failed) + " passed")
    print(json.dumps({"suite": "check_serve", "passed": passed, "failed": failed, "failures": failures}))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
