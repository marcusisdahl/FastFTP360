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
def server(count=1):
    port = free_port()
    process = subprocess.Popen([str(ROOT / "build/host/host-server.exe"), str(port), str(count)],
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


def client(port, direction=1, seconds=1, cancel=None):
    args = [str(ROOT / "build/host/client-cli.exe"), str(port), str(direction), str(seconds)]
    if cancel is not None:
        args.append(str(cancel))
    return subprocess.run(args, text=True, capture_output=True, timeout=25, creationflags=subprocess.CREATE_NO_WINDOW)


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(8)
    return s


def request(mode=1):
    return [MAGIC, 1, mode, 1, CHUNK, CHUNK, 1, 123]


for mode in (1, 2):
    with server() as (port, host):
        result = client(port, mode, 2)
        output = host.communicate(timeout=5)[0]
        print("Client:", result.stdout.strip(), "Server:", output.strip(), flush=True)
        check("C# / C++ direction %d receiver totals agree" % mode,
              result.returncode == 0 and result.stdout.strip() == output.strip() and "PASS" in output)

with server(2) as (port, host):
    cancelled = client(port, 1, 10, 300)
    retry = client(port, 2)
    output = host.communicate(timeout=5)[0]
    check("cancel discards incomplete run and next connection succeeds",
          cancelled.returncode == 2 and retry.returncode == 0 and output.startswith("ERROR") and "PASS" in output)

for field, value in ((0, 0), (1, 99), (2, 3), (3, 0), (3, 61), (4, 1), (5, 0), (6, 0), (6, 65)):
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


stub_test("client rejects truncated response", lambda s, f: s.sendall(b"NB"), 1, "ended the transfer early")
stub_test("client rejects wrong protocol", lambda s, f: s.sendall(b"\0" * 32), 1, "different protocol")
stub_test("client rejects corrupt integrity data", corrupt, 2, "integrity check FAILED")
stub_test("client rejects forged receiver result", bad_result, 1, "Receiver totals")
print("%d checks passed. Console execution remains untested." % passed)
