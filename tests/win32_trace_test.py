import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe = sys.argv[1]
env = os.environ.copy()
env.pop("RAPPORT_TRACE", None)
subprocess.run([exe], env=env, check=True)
with tempfile.TemporaryDirectory() as folder:
    final = Path(folder, "exit.json")
    snap = Path(folder, "dump.json")
    subprocess.run([exe, final, snap], env={**env, "RAPPORT_TRACE": str(final)}, check=True)
    for file, expected in [(snap, 8), (final, 10)]:
        data = json.loads(file.read_text())
        events = data["traceEvents"]
        assert len(events) == expected and data["qpcFrequency"] > 0
        assert data["omittedEvents"] == data["unrecordedThreads"] == 0
        stacks = {}
        burst = {e["args"]["burst"] for e in events if e["name"] != "Present"}
        assert len(burst) == 1 and 0 not in burst
        for e in events:
            stack = stacks.setdefault(e["tid"], [])
            key = (e["name"], e["args"]["arg"], e["args"]["burst"])
            if e["ph"] == "B":
                stack.append(key)
            else:
                assert e["ph"] == "E" and stack.pop() == key
        assert len(stacks) == 2 and not any(stacks.values())
        assert max(e["args"]["qpc"] for e in events) > 0
        orders = [[(e["name"], e["ph"]) for e in events if e["tid"] == tid] for tid in stacks]
        assert [("readback.submit", "B"), ("readback.complete", "B"),
                ("readback.complete", "E"), ("readback.submit", "E")] in orders
        assert any(order[:4] == [("window.resize", "B"), ("layout", "B"),
                                 ("layout", "E"), ("window.resize", "E")] for order in orders)

    wrapped = Path(folder, "wrapped.json")
    subprocess.run([exe, wrapped], env={**env, "RAPPORT_TRACE": str(wrapped)}, check=True)
    data = json.loads(wrapped.read_text())
    assert len(data["traceEvents"]) == 65536 and data["omittedEvents"] == 160000 - 65536
    assert data["traceEvents"][-1]["args"]["arg"] == hex(39999)

    hooked = Path(folder, "messages.json")
    subprocess.run([exe, "hooks", "unused", "unused"],
                   env={**env, "RAPPORT_TRACE": str(hooked)}, check=True)
    data = json.loads(hooked.read_text())
    events = data["traceEvents"]
    assert data["omittedEvents"] == 0
    assert any(e["name"] == "message" and e["args"]["message"] == 15
               and e["args"]["inSize"] == e["args"]["inBatch"] == 1 for e in events)
    assert any(e["name"] == "message" and e["args"]["message"] == 791 for e in events)
    stack = []
    for e in events:
        key = (e["name"], e["args"]["arg"], e["args"]["message"])
        if e["ph"] == "B":
            stack.append(key)
        else:
            assert stack.pop() == key
    assert not stack
