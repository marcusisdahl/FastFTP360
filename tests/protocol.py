"""Cross-language loopback tests; no Xbox, disk benchmark or network adapter required."""
import contextlib
import socket
import struct
import subprocess
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAGIC, CHUNK, MIB = 0x4E423336, 65536, 1048576
passed = 0


def check(name, condition):
    global passed
    assert condition, name
    passed += 1
    print("PASS " + name, flush=True)


def exact(s, count):
    data = bytearray()
    while len(data) < count:
        part = s.recv(count - len(data))
        if not part:
            raise EOFError("short stream")
        data.extend(part)
    return bytes(data)


def pattern(seed):
    state = seed or 1
    result = bytearray()
    for _ in range(CHUNK):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        result.append(state & 255)
    return bytes(result)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@contextlib.contextmanager
def server(count=1, lan_shim=False, fixed_port=None):
    port = fixed_port if fixed_port is not None else free_port()
    executable = "lan-host-server.exe" if lan_shim else "host-server.exe"
    process = subprocess.Popen([str(ROOT / "build/host" / executable), str(port), str(count)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        # Readiness is observed without consuming one of the server's connections.
        ready = []
        thread = threading.Thread(target=lambda: ready.append(process.stdout.readline()), daemon=True)
        thread.start()
        thread.join(5)
        assert ready and ready[0].startswith("READY"), "host server did not start"
        yield port, process
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=3)


def client(port, direction=1, seconds=1, cancel=None, buffer=None, verify=False):
    args = [str(ROOT / "build/host/client-cli.exe"), str(port), str(direction), str(seconds)]
    if cancel is not None or buffer is not None or verify:
        args.append(str(cancel if cancel is not None else -1))
    if buffer is not None or verify:
        args.append(str(buffer if buffer is not None else 65536))
    if verify:
        args.append("verify")
    return subprocess.run(args, text=True, capture_output=True, timeout=25, creationflags=subprocess.CREATE_NO_WINDOW)


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(8)
    return s


def request(mode=1):
    return [MAGIC, 1, mode, 1, CHUNK, CHUNK, 1, 123]


VERIFY_ALL = 0x100


for mode in (1, 2):
    with server() as (port, host):
        result = client(port, mode, 2)
        output = host.communicate(timeout=5)[0]
        print("Client:", result.stdout.strip(), "Server:", output.strip(), flush=True)
        check("C# / C++ direction %d receiver totals agree" % mode,
              result.returncode == 0 and result.stdout.strip() == output.strip() and "PASS" in output)

for mode in (1, 2):
    with server(lan_shim=True) as (port, host):
        result = client(port, mode)
        output = host.communicate(timeout=5)[0]
        check("direction %d works when LAN and buffer changes on accepted sockets are rejected" % mode,
              result.returncode == 0 and result.stdout.strip() == output.strip() and "PASS" in output)

for buffer in (0, 16384, 32768, 65536, 131072):
    for mode in (1, 2):
        with server(lan_shim=True) as (port, host):
            result = client(port, mode, buffer=buffer)
            output = host.communicate(timeout=5)[0]
            check("PC buffer %d / direction %d with fixed server buffers and accurate metadata" % (buffer, mode),
                  result.returncode == 0 and "PASS" in output)

for mode in (1, 2):
    with server() as (port, host):
        result = client(port, mode, 2, verify=True)
        output = host.communicate(timeout=5)[0]
        lines = result.stdout.strip().splitlines()
        verified = lines[-1].split() if lines else []
        check("direction %d verifies every timed byte at the receiver" % mode,
              result.returncode == 0 and "PASS" in output and len(verified) == 4 and verified[0] == "VERIFIED" and
              verified[1] == verified[3] and int(verified[1]) > 0)

with server(2) as (port, host):
    runs = [None, None]
    def parallel_client(index, mode):
        runs[index] = client(port, mode, 2)
    workers = [threading.Thread(target=parallel_client, args=(i, mode)) for i, mode in enumerate((1, 1))]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join(30)
    output = host.communicate(timeout=5)[0]
    check("two parallel streams complete on one listener",
          all(r is not None and r.returncode == 0 for r in runs) and output.count("PASS") == 2)

restart_port = free_port()
for run in (1, 2):
    with server(lan_shim=True, fixed_port=restart_port) as (port, host):
        result = client(port, 2)
        output = host.communicate(timeout=5)[0]
        check("same-port listener start %d succeeds immediately" % run, result.returncode == 0 and "PASS" in output)

with server(2) as (port, host):
    cancelled = client(port, 1, 10, 300)
    retry = client(port, 2)
    output = host.communicate(timeout=5)[0]
    check("cancel discards incomplete run and next connection succeeds",
          cancelled.returncode == 2 and retry.returncode == 0 and output.startswith("ERROR") and "PASS" in output)

for field, value in ((0, 0), (1, 99), (2, 3), (2, 0x201), (2, VERIFY_ALL), (3, 0), (3, 61), (4, 1), (5, 1),
                     (6, 0), (6, 65)):
    with server() as (port, host), connect(port) as s:
        fields = request()
        fields[field] = value
        s.sendall(struct.pack(">8I", *fields))
        response = struct.unpack(">8I", exact(s, 32))
        check("invalid request field %d value %d rejected" % (field, value), response[:3] == (MAGIC, 1, 1))

with server() as (port, host), connect(port) as s:
    for byte in struct.pack(">8I", *request()):
        s.sendall(bytes([byte]))
    check("fragmented request accepted", struct.unpack(">8I", exact(s, 32))[:3] == (MAGIC, 1, 0))
    s.sendall(b"\0" * CHUNK)
    output = host.communicate(timeout=5)[0]
    check("server rejects corrupt integrity data", "Integrity check failed" in output)

with server() as (port, host), connect(port) as s:
    s.sendall(struct.pack(">8I", *request()))
    exact(s, 32)
    block = pattern(123)
    for _ in range(16):
        s.sendall(block)
    assert exact(s, 4) == b"\0" * 4
    for _ in range(16):
        s.sendall(block)
    assert exact(s, 4) == b"\0" * 4
    s.sendall(block)
    s.shutdown(socket.SHUT_WR)
    output = host.communicate(timeout=5)[0]
    check("clean disconnect during measurement is not a successful run", "valid finish record" in output)

with server() as (port, host), connect(port) as s:
    s.sendall(b"NB")
    s.shutdown(socket.SHUT_WR)
    check("truncated request rejected", "Unexpected end" in host.communicate(timeout=5)[0])

with server() as (port, host), connect(port) as s:
    s.sendall(struct.pack(">8I", *request(2)))
    exact(s, 32)
    assert exact(s, MIB) == pattern(123) * 16
    s.sendall(b"\0" * 4)
    exact(s, MIB)
    s.sendall(b"\0" * 4)
    while s.recv(CHUNK):
        pass
    s.sendall(b"\0" * 64)
    check("server rejects forged receiver result", "Receiver result did not match" in host.communicate(timeout=5)[0])


with server() as (port, host), connect(port) as s:
    fields = request()
    fields[2] = 1 | VERIFY_ALL
    s.sendall(struct.pack(">8I", *fields))
    exact(s, 32)
    block = pattern(123)
    for _ in range(2):
        for _ in range(16):
            s.sendall(block)
        assert exact(s, 4) == b"\0" * 4
    s.sendall(block)
    s.sendall(block[:1000] + bytes([block[1000] ^ 1]) + block[1001:])
    output = host.communicate(timeout=5)[0]
    check("full data check rejects one corrupt timed byte", "Integrity check failed during timed transfer" in output)


def stub_test(name, handler, mode, expected):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        errors = []
        def serve_stub():
            try:
                peer, _ = listener.accept()
                with peer:
                    peer.settimeout(8)
                    fields = struct.unpack(">8I", exact(peer, 32))
                    handler(peer, fields)
            except Exception as error:
                errors.append(error)
        worker = threading.Thread(target=serve_stub, daemon=True)
        worker.start()
        result = client(listener.getsockname()[1], mode)
        worker.join(10)
        check(name, not worker.is_alive() and not errors and result.returncode == 1 and expected in result.stdout)


def response(s, fields):
    s.sendall(struct.pack(">8I", MAGIC, 1, 0, CHUNK, CHUNK, CHUNK, MIB, fields[6]))


def corrupt(s, fields):
    response(s, fields)
    s.sendall(b"\0" * CHUNK)


def bad_result(s, fields):
    response(s, fields)
    exact(s, MIB)
    s.sendall(b"\0" * 4)
    exact(s, MIB)
    s.sendall(b"\0" * 4)
    while s.recv(CHUNK):
        pass
    s.sendall(b"\0" * 64)


stub_test("client identifies a truncated handshake", lambda s, f: s.sendall(b"NB"), 1, "during negotiating (2/32 bytes)")
stub_test("client rejects wrong protocol", lambda s, f: s.sendall(b"\0" * 32), 1, "different protocol")
stub_test("client rejects corrupt integrity data", corrupt, 2, "integrity check FAILED")
stub_test("client rejects forged receiver result", bad_result, 1, "Receiver totals")
print("%d checks passed. Console execution remains untested." % passed)
