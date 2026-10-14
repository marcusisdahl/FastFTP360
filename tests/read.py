"""Read benchmark host tests: the Xbox app's read engine (shared/readbench.cpp),
built as build/host/read-host, against local files and a controllable NetISO
server (tests/netiso_server.py). No Xbox, NetISO plugin or ConnectX needed.

Optional: set NETISO_SRV to a built tuxuser/netiso-srv binary to also run the
reference server checks."""
import contextlib
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/host" / ("read-host.exe" if os.name == "nt" else "read-host")
MIB = 1048576
SEQ64K, SEQ256K, SEQ1M, RAND4K, RAND16K, RAND64K = range(6)
REQUEST = [65536, 262144, 1048576, 4096, 16384, 65536]
passed = 0
FLAGS = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def check(name, condition, detail=""):
    global passed
    assert condition, name + (": " + str(detail) if detail else "")
    passed += 1
    print("PASS " + name, flush=True)


def run(*args, timeout=60, stdin=None):
    output = subprocess.run([str(EXE), *map(str, args)], input=stdin, capture_output=True, text=True, timeout=timeout,
                            **FLAGS).stdout
    return output


def results(output):
    found = []
    for line in output.splitlines():
        if line.startswith("RESULT "):
            fields = {}
            head, _, error = line[7:].partition(" error=")
            for item in head.split():
                key, _, value = item.partition("=")
                fields[key] = value
            fields["error"] = error
            found.append(fields)
    return found


def result(output):
    found = results(output)
    assert found, "no RESULT in: " + output
    return found[0]


def fixture_word(seed, word):
    """Independent Python copy of nb::FixtureWord; the expected content of a fixture."""
    m = 0xFFFFFFFF
    x = ((word & m) * 0x9E3779B1) ^ ((word >> 32) * 0xC2B2AE35) ^ (seed * 0x85EBCA77) ^ 0x27D4EB2F
    x &= m
    x ^= x >> 16
    x = (x * 0x7FEB352D) & m
    x ^= x >> 15
    x = (x * 0x846CA68B) & m
    x ^= x >> 16
    return x


def fixture_bytes(seed, offset, length):
    first, last = offset // 4, (offset + length + 3) // 4
    data = b"".join(struct.pack(">I", fixture_word(seed, w)) for w in range(first, last))
    return data[offset - first * 4:offset - first * 4 + length]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@contextlib.contextmanager
def server(folder, *options, command=None):
    port = free_port()
    argv = command(port) if command else [sys.executable, "-I", str(ROOT / "tests/netiso_server.py"), str(folder),
                                          "--bind", "127.0.0.1", "--port", str(port), *options]
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **FLAGS)
    try:
        if command:
            deadline = time.time() + 10
            while time.time() < deadline:
                with socket.socket() as probe:
                    if probe.connect_ex(("127.0.0.1", port)) == 0:
                        break
                time.sleep(0.1)
        else:
            ready = []
            thread = threading.Thread(target=lambda: ready.append(process.stdout.readline()), daemon=True)
            thread.start()
            thread.join(10)
            assert ready and ready[0].startswith("READY"), "netiso test server did not start"
        yield port
    finally:
        process.kill()
        process.wait(timeout=5)


def netiso(port, iso="-", workload=SEQ64K, seconds=2, seed=7, verify=1, timeout=3, cancel=0, repeat=1, wall=60):
    return run("netiso", "127.0.0.1", port, iso, workload, seconds, seed, verify, timeout, cancel, repeat, timeout=wall)


def ok(r, verify="2"):
    return r["status"] == "completed" and r["verify"] == verify and int(r["ops"]) > 0 and \
        r["failed"] == r["short"] == r["timeouts"] == r["integrity"] == "0"


with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    isos = temp / "isos"
    isos.mkdir()
    fixture = isos / "FFTPFIX-0000002A.iso"
    run("fixture", fixture, 48 * MIB, 0x2A)
    data = fixture.read_bytes()

    # Fixture formula: the C++ generator matches an independent Python version.
    rng = random.Random(1)
    spots = [0, 1, 2, 3, 4093, len(data) - 9] + [rng.randrange(len(data) - 64) for _ in range(40)]
    check("fixture bytes match the independent formula",
          len(data) == 48 * MIB and all(data[o:o + 9] == fixture_bytes(0x2A, o, 9) for o in spots))

    # Local file backend.
    for workload in range(6):
        r = result(run("file", fixture, workload, 1, 9, 1))
        check("local %s completes with known data checked" % ["seq 64K", "seq 256K", "seq 1M", "rand 4K", "rand 16K", "rand 64K"][workload],
              ok(r) and int(r["bytes"]) == int(r["ops"]) * REQUEST[workload], r)
    plain = temp / "plain.bin"
    plain.write_bytes(os.urandom(4 * MIB))
    r = result(run("file", plain, RAND4K, 1, 1, 1))
    check("ordinary file is byte-count checked only", ok(r, verify="1"), r)
    r = result(run("file", plain, SEQ64K, 1, 1, 0))
    check("check off is reported as off", ok(r, verify="0"), r)
    small = temp / "small.bin"
    small.write_bytes(b"x" * 100000)
    r = result(run("file", small, SEQ1M, 1, 1, 1))
    check("file smaller than one read fails with an explanation", r["status"] == "failed" and "smaller than one 1024 KiB read" in r["error"], r)
    r = result(run("file", small, SEQ64K, 1, 1, 1))
    check("sequential reads wrap inside a small file", ok(r, verify="1") and int(r["wraps"]) > 0, r)
    r = result(run("file", temp / "missing.bin", SEQ64K, 1, 1, 1))
    check("missing file fails gracefully", r["status"] == "failed" and "Cannot open" in r["error"], r)
    r = result(run("file", temp, SEQ64K, 1, 1, 1))
    check("a folder is refused", r["status"] == "failed" and "Cannot open" in r["error"], r)
    bad = temp / "FFTPFIX-0000002A.bin"
    corrupted = bytearray(data[:8 * MIB])
    corrupted[5 * MIB + 77] ^= 1
    bad.write_bytes(corrupted)
    r = result(run("file", bad, SEQ1M, 2, 1, 1))
    check("one wrong byte in a fixture is an integrity failure", r["status"] == "failed" and r["integrity"] == "1" and
          str(5 * MIB + 77) in r["error"], r)
    r = result(run("file", bad, SEQ1M, 2, 1, 0))
    check("with the check off the same file is not called verified", r["status"] == "completed" and r["verify"] == "0", r)
    started = time.time()
    r = result(run("file", fixture, RAND4K, 30, 1, 1, 1.5))
    check("cancel stops a local run promptly", r["status"] == "cancelled" and time.time() - started < 5, r)
    a = result(run("file", fixture, RAND16K, 1, 123, 1))
    b = result(run("file", fixture, RAND16K, 1, 123, 1))
    check("random runs with the same seed both complete", ok(a) and ok(b))

    # Settings in FastFTP.ini.
    ini = temp / "FastFTP.ini"
    ini.write_text("login=on\r\nuser=xboxftp\r\npassword=secret\r\nread_folder=Games\\Bench\r\nconnectx_folder=ConnectX:\\Games\r\n"
                   "netiso_server=192.168.1.20:4400\r\nread_seed=0x2A\r\n")
    out = run("settings", ini)
    check("read settings parse and round-trip", "folder=Games\\Bench connectx=ConnectX:\\Games server=192.168.1.20:4400 port=4400 seed=42 roundtrip" in out, out)
    check("the password is not part of the read settings", "secret" not in out, out)
    ini.write_text("read_folder=..\\Flash\r\nread_folder=C:\\x\r\nconnectx_folder=nocolon\r\nnetiso_server=300.1.1.1\r\nnetiso_port=99999\r\n")
    out = run("settings", ini)
    check("invalid read settings keep the defaults", "folder=FastFTP-Bench connectx=- server=- port=4323 seed=1 roundtrip" in out, out)

    # Latency histogram.
    out = run("percentiles", *range(1, 101))
    check("percentiles are exact below 64 us", "p50=50 p95=95 p99=99 max=100" in out, out)
    values = [rng.randrange(1000, 2000000) for _ in range(5000)]
    out = run("percentiles", stdin=" ".join(map(str, values))).split()
    stats = dict(item.split("=") for item in out[1:])
    exact = sorted(values)
    for name, fraction in (("p50", 0.5), ("p95", 0.95), ("p99", 0.99)):
        true_value = exact[max(0, -(-int(fraction * len(values) * 1000) // 1000) - 1)]
        check("%s within 3.2%% of exact" % name, true_value <= int(stats[name]) <= true_value * 1.032, (name, true_value, stats[name]))
    check("maximum and mean are exact", int(stats["max"]) == max(values) and abs(float(stats["mean"]) - sum(values) / len(values)) < 0.01)

    # NetISO protocol against the controlled server.
    (isos / "data.iso").write_bytes(os.urandom(8 * MIB))
    (isos / "tiny.iso").write_bytes(b"\0" * 3 * 2048)
    with server(isos) as port:
        out = run("list", "127.0.0.1", port)
        check("ISO list matches the folder", "LISTED 3" in out and "ISO FFTPFIX-0000002A.iso" in out and "ISO tiny.iso" in out, out)
        for workload in range(6):
            r = result(netiso(port, "FFTPFIX-0000002A.iso", workload, seconds=1))
            check("NetISO %s completes with known data checked" % ["seq 64K", "seq 256K", "seq 1M", "rand 4K", "rand 16K", "rand 64K"][workload],
                  ok(r) and int(r["bytes"]) == int(r["ops"]) * REQUEST[workload] and int(r["size"]) == 48 * MIB, r)
        r = result(netiso(port, "data.iso", RAND64K, seconds=1))
        check("ordinary ISO is byte-count checked only", ok(r, verify="1"), r)
        r = result(netiso(port, "tiny.iso", SEQ64K))
        check("ISO smaller than one read fails with an explanation", r["status"] == "failed" and "smaller than one 64 KiB read" in r["error"], r)
        r = result(netiso(port, "missing.iso"))
        check("unknown ISO name fails gracefully", r["status"] == "failed" and "could not mount" in r["error"], r)
        out = netiso(port, "FFTPFIX-0000002A.iso", RAND4K, seconds=1, repeat=5)
        runs = results(out)
        descriptors = [line for line in out.splitlines() if line.startswith("DESCRIPTORS")]
        same = descriptors and (descriptors[0].split()[1].split("=")[1] == descriptors[0].split()[2].split("=")[1] or os.name == "nt")
        check("five repeated runs complete and release their sockets", len(runs) == 5 and all(ok(x) for x in runs) and same, out)
        started = time.time()
        r = result(netiso(port, "FFTPFIX-0000002A.iso", SEQ64K, seconds=30, cancel=1.5))
        check("cancel stops a NetISO run promptly", r["status"] == "cancelled" and time.time() - started < 5, r)

    ambiguous = temp / "ambiguous"
    ambiguous.mkdir()
    (ambiguous / "a.iso").write_bytes(b"\0" * MIB)
    (ambiguous / "data.iso").write_bytes(b"\0" * MIB)
    with server(ambiguous) as port:
        r = result(netiso(port, "a.iso"))
        check("a name that ends another ISO's name is refused as ambiguous", r["status"] == "failed" and "Ambiguous" in r["error"], r)

    with server(isos, "--fault", "split") as port:
        r = result(netiso(port, "FFTPFIX-0000002A.iso", SEQ256K, seconds=1, timeout=5))
        check("replies in 7-byte pieces are reassembled and verified", ok(r), r)

    def fault(name, workload=SEQ64K, timeout=2, cancel=0, wall=60, *extra):
        with server(isos, "--fault", name, "--nth", "3", *extra) as port:
            started = time.time()
            r = result(netiso(port, "FFTPFIX-0000002A.iso", workload, seconds=2, timeout=timeout, cancel=cancel, wall=wall))
            return r, time.time() - started

    r, _ = fault("truncate")
    check("a reply cut short by a disconnect is a short read, not data", r["status"] == "failed" and r["short"] == "1" and r["integrity"] == "0", r)
    r, took = fault("truncate-stall", timeout=2)
    check("a reply that stops halfway times out", r["status"] == "failed" and r["timeouts"] == "1" and took < 20, (r, took))
    r, took = fault("stall", timeout=2)
    check("no reply at all times out", r["status"] == "failed" and r["timeouts"] == "1" and took < 20, (r, took))
    r, took = fault("stall", timeout=30, cancel=1.5)
    check("cancel interrupts a stalled read", r["status"] == "cancelled" and took < 6, (r, took))
    r, _ = fault("corrupt")
    check("a flipped byte from the server is an integrity failure", r["status"] == "failed" and r["integrity"] == "1", r)
    r, _ = fault("disconnect")
    check("a server disconnect ends the run as failed", r["status"] == "failed" and r["short"] == "1", r)
    r, _ = fault("trywrite", SEQ1M, 2)
    check("a reply truncated like netiso-srv's try_write fails, not corrupts", r["status"] == "failed" and r["timeouts"] == "1" and
          r["integrity"] == "0", r)
    r, _ = fault("bad-sector-size")
    check("an unexpected sector size is rejected", r["status"] == "failed" and "sector size 4096" in r["error"], r)
    r, _ = fault("bad-mount")
    check("a malformed mount reply is rejected", r["status"] == "failed" and "Malformed mount reply" in r["error"], r)
    r, _ = fault("bad-name")
    check("a malformed ISO name is rejected", r["status"] == "failed" and "Malformed ISO name" in r["error"], r)
    r, _ = fault("long-name")
    check("an over-long ISO name is reported", r["status"] == "failed" and "256 or more bytes" in r["error"], r)
    r, _ = fault("not-netiso")
    check("a server that is not NetISO is detected", r["status"] == "failed" and "Not a NetISO server" in r["error"], r)
    r, _ = fault("close-after-ping")
    check("a server that hangs up during setup fails cleanly", r["status"] == "failed", r)

    port = free_port()
    started = time.time()
    r = result(netiso(port))
    check("no server listening fails quickly", r["status"] == "failed" and time.time() - started < 10, r)

    # The reference server, if one is available.
    reference = os.environ.get("NETISO_SRV")
    if reference:
        refdir = temp / "reference"
        refdir.mkdir()
        shutil.copy(fixture, refdir / fixture.name)
        # netiso-srv always listens on port 4323.
        process = subprocess.Popen([reference, str(refdir)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **FLAGS)
        try:
            time.sleep(1)
            for workload in (SEQ64K, RAND4K, RAND64K):
                r = result(netiso(4323, fixture.name, workload, seconds=1))
                check("reference netiso-srv: workload %d reads known data" % workload, ok(r), r)
        finally:
            process.kill()
            process.wait(timeout=5)

print("%d read checks passed" % passed)
