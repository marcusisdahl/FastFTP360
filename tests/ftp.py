"""FTP server tests on Windows loopback with Python's ftplib; no Xbox required."""
import contextlib
import ftplib
import io
import os
import socket
import subprocess
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIB = 1048576
passed = 0


def check(name, condition):
    global passed
    assert condition, name
    passed += 1
    print("PASS " + name, flush=True)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def pattern(seed, count):
    state = seed or 1
    block = bytearray()
    for _ in range(65536):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        block.append(state & 255)
    return (bytes(block) * (count // 65536 + 1))[:count]


@contextlib.contextmanager
def server(folder, sessions=1, credentials=()):
    port = free_port()
    process = subprocess.Popen([str(ROOT / "build/host/ftp-host.exe"), str(port), str(folder), str(sessions), *credentials],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        ready = []
        thread = threading.Thread(target=lambda: ready.append(process.stdout.readline()), daemon=True)
        thread.start()
        thread.join(5)
        assert ready and ready[0].startswith("READY"), "ftp host did not start"
        yield port, process
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)


def login(port):
    ftp = ftplib.FTP()
    ftp.connect("127.0.0.1", port, timeout=20)
    ftp.login("anyone", "anything")
    return ftp


with tempfile.TemporaryDirectory() as temp:
    served = Path(temp) / "served"
    served.mkdir()
    (Path(temp) / "outside.txt").write_bytes(b"secret")
    payload = os.urandom(5 * MIB + 12345)

    with server(served) as (port, host):
        ftp = login(port)
        check("login accepts any user and password", ftp.pwd() == "/")
        check("top level lists the drive folders", sorted(ftp.nlst()) == ["HDD", "NULL"])
        check("MKD creates a folder", ftp.mkd("/HDD/sub") == "/HDD/sub" and (served / "sub").is_dir())
        ftp.cwd("/HDD/sub")
        check("CWD changes folder", ftp.pwd() == "/HDD/sub")
        reply = ftp.storbinary("STOR big.bin", io.BytesIO(payload))
        check("STOR writes the exact bytes", (served / "sub" / "big.bin").read_bytes() == payload and "MiB/s" in reply)
        check("SIZE reports the file size", ftp.size("big.bin") == len(payload))
        received = bytearray()
        ftp.retrbinary("RETR big.bin", received.extend)
        check("RETR returns the exact bytes", bytes(received) == payload)
        lines = []
        ftp.retrlines("LIST", lines.append)
        check("LIST shows the file and size", any(line.endswith(" big.bin") and " %d " % len(payload) in line for line in lines))
        ftp.storbinary("STOR empty.bin", io.BytesIO(b""))
        empty = bytearray()
        ftp.retrbinary("RETR empty.bin", empty.extend)
        check("empty files upload and download", (served / "sub" / "empty.bin").stat().st_size == 0 and not empty)
        ftp.rename("big.bin", "renamed.bin")
        check("RNFR/RNTO renames", (served / "sub" / "renamed.bin").exists())
        ftp.delete("renamed.bin")
        ftp.delete("empty.bin")
        check("DELE removes files", not any((served / "sub").iterdir()))
        ftp.cwd("..")
        ftp.rmd("sub")
        check("CDUP and RMD remove the folder", ftp.pwd() == "/HDD" and not (served / "sub").exists())
        try:
            ftp.retrbinary("RETR /HDD/../../outside.txt", bytearray().extend)
            escaped = True
        except ftplib.error_perm:
            escaped = False
        check("dot-dot cannot leave the served folders", not escaped)
        try:
            ftp.retrbinary("RETR /HDD/x\\..\\..\\outside.txt", bytearray().extend)
            escaped = True
        except ftplib.error_perm:
            escaped = False
        check("backslash paths are rejected", not escaped)
        reply = ftp.storbinary("STOR /NULL/discard.bin", io.BytesIO(payload))
        check("NULL folder discards uploads", "MiB/s" in reply and not list(served.iterdir()))
        streamed = bytearray()
        ftp.retrbinary("RETR /NULL/16MB.bin", streamed.extend)
        check("NULL folder streams n MiB of pattern", len(streamed) == 16 * MIB and bytes(streamed[:65536 * 2]) == pattern(0x46545021, 65536 * 2))
        check("NULL sample files are listed", "256MB.bin" in ftp.nlst("/NULL"))
        check("EPSV is answered", ftp.sendcmd("EPSV").startswith("229 "))
        try:
            ftp.sendcmd("PORT 127,0,0,1,4,1")
            active = True
        except ftplib.error_perm:
            active = False
        check("active mode is refused clearly", not active)
        ftp.quit()
        output = host.communicate(timeout=10)[0]
        check("server reports completed transfers and stops", "DONE STOR big.bin bytes=%d" % len(payload) in output and "STOPPED" in output)

    with server(served, sessions=3) as (port, host):
        results = []

        def worker(index):
            data = os.urandom(3 * MIB + index)
            client = login(port)
            client.storbinary("STOR /HDD/p%d.bin" % index, io.BytesIO(data))
            back = bytearray()
            client.retrbinary("RETR /HDD/p%d.bin" % index, back.extend)
            client.quit()
            results.append(bytes(back) == data)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(3)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(60)
        host.communicate(timeout=10)
        check("three simultaneous sessions transfer correctly", results == [True, True, True])

    with server(served, sessions=3, credentials=("xboxftp", "xboxftp")) as (port, host):
        ftp = ftplib.FTP()
        ftp.connect("127.0.0.1", port, timeout=20)
        try:
            ftp.login("xboxftp", "wrong")
            refused = False
        except ftplib.error_perm as error:
            refused = str(error).startswith("530")
        check("a wrong password is refused", refused)
        ftp.quit()
        ftp = ftplib.FTP()
        ftp.connect("127.0.0.1", port, timeout=20)
        try:
            ftp.sendcmd("PWD")
            blocked = False
        except ftplib.error_perm as error:
            blocked = str(error).startswith("530")
        check("commands before login are refused", blocked)
        ftp.quit()
        ftp = ftplib.FTP()
        ftp.connect("127.0.0.1", port, timeout=20)
        ftp.login("xboxftp", "xboxftp")
        check("the configured user name and password log in", ftp.pwd() == "/" and sorted(ftp.nlst()) == ["HDD", "NULL"])
        ftp.quit()
        host.communicate(timeout=10)

print("%d FTP checks passed. Console execution remains untested." % passed)
