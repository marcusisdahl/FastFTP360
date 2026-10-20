"""FTP server tests on loopback with Python's ftplib; no Xbox required.

On Windows, build.ps1 -Tests builds build/host/ftp-host.exe and runs this. On
Linux or macOS, build the same server first:
  g++ -std=c++11 -O1 -include tests/ftp_posix.h tests/ftp_host.cpp shared/ftp.cpp shared/bench.cpp -o build/host/ftp-host -lpthread
"""
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
HOST = ROOT / ("build/host/ftp-host.exe" if os.name == "nt" else "build/host/ftp-host")
WINDOWLESS = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
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
    process = subprocess.Popen([str(HOST), str(port), str(folder), str(sessions), *credentials],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **WINDOWLESS)
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

    with server(served, sessions=1, credentials=("marcus", "S3cret!=#")) as (port, host):
        ftp = ftplib.FTP()
        ftp.connect("127.0.0.1", port, timeout=20)
        ftp.login("marcus", "S3cret!=#")
        check("a user-chosen name and password log in", ftp.pwd() == "/")
        ftp.storbinary("STOR /HDD/log.bin", io.BytesIO(b"hello"))
        ftp.rename("/HDD/log.bin", "/HDD/log2.bin")
        ftp.delete("/HDD/log2.bin")
        ftp.quit()
        output = host.communicate(timeout=10)[0]
        log = [line[4:] for line in output.splitlines() if line.startswith("LOG ")]
        expected = ["Connected", "Logged in as marcus", "Upload /HDD/log.bin, 5 bytes in", "Renamed /HDD/log.bin to /HDD/log2.bin",
                    "Deleted /HDD/log2.bin", "Disconnected"]
        check("the log records login, upload, rename, delete and disconnect with the client address",
              len(log) == len(expected) and all(line.startswith("127.0.0.1  " + text) for line, text in zip(log, expected)))

    class Raw:
        """A control connection driven by hand, for what ftplib cannot do."""

        def __init__(self, port):
            self.sock = socket.create_connection(("127.0.0.1", port), timeout=20)
            self.file = self.sock.makefile("rb")
            self.reply()
            self.command("USER anyone")
            self.command("PASS anything")

        def reply(self):
            line = self.file.readline().decode("latin-1").rstrip("\r\n")
            while len(line) > 3 and line[3] == "-":
                line = self.file.readline().decode("latin-1").rstrip("\r\n")
            return line

        def command(self, text):
            self.sock.sendall(text.encode("latin-1") + b"\r\n")
            return self.reply()

        def hang_up(self):
            self.file.close()
            self.sock.close()

        def passive(self):
            reply = self.command("PASV")
            numbers = reply[reply.index("(") + 1:reply.index(")")].split(",")
            return reply, int(numbers[4]) * 256 + int(numbers[5])

    def leftovers(folder):
        return [p.name for p in folder.iterdir() if p.name.startswith("FastFTP360-")]

    with server(served, sessions=0) as (port, host):
        ftp = login(port)
        reply = ftp.sendcmd("PASV")
        check("PASV announces the address the client connected to", "(127,0,0,1," in reply)

        # Finding 1: long names and paths get an error reply instead of crashing the app.
        try:
            ftp.sendcmd("SIZE /HDD/" + "a" * 300)
            answered = ""
        except ftplib.error_perm as error:
            answered = str(error)
        check("a 300-character name is refused, not a crash", answered.startswith("550") and ftp.voidcmd("NOOP").startswith("200"))
        try:
            ftp.sendcmd("RNFR /HDD/" + "e/" * 280 + "x")
            answered = ""
        except ftplib.error_perm as error:
            answered = str(error)
        check("a 560-character rename path is refused, not a crash", answered.startswith("550") and ftp.voidcmd("NOOP").startswith("200"))
        try:
            ftp.storbinary("STOR /HDD/" + "b" * 300, io.BytesIO(b"x"))
            answered = ""
        except ftplib.error_perm as error:
            answered = str(error)
        check("STOR with an over-long name says the name is too long", answered.startswith("553") and "too long" in answered)
        deep = "c" * 200
        ftp.mkd("/HDD/" + deep)
        ftp.cwd("/HDD/" + deep)
        try:
            ftp.sendcmd("SIZE " + "d" * 250)
            answered = ""
        except ftplib.error_perm as error:
            answered = str(error)
        check("a long relative path from a deep folder is refused", answered.startswith("550") and ftp.pwd() == "/HDD/" + deep)
        ftp.cwd("/HDD")
        ftp.rmd(deep)
        try:
            ftp.sendcmd("NOOP " + "x" * 700)
            answered = ""
        except ftplib.error_perm as error:
            answered = str(error)
        check("an over-long command line gets 500 and the session goes on", answered.startswith("500") and ftp.voidcmd("NOOP").startswith("200"))
        long_name = "n" * 100 + ".bin"
        ftp.storbinary("STOR /HDD/" + long_name, io.BytesIO(b"long"))
        check("names longer than FATX's 42 characters still work where the drive allows them",
              (served / long_name).read_bytes() == b"long")
        ftp.delete("/HDD/" + long_name)

        # Finding 2: a failed overwrite keeps the old file.
        original = os.urandom(MIB + 7)
        ftp.storbinary("STOR /HDD/keep.bin", io.BytesIO(original))
        raw = Raw(port)
        _, data_port = raw.passive()
        data = socket.create_connection(("127.0.0.1", data_port), timeout=20)
        check("an overwrite starts", raw.command("STOR /HDD/keep.bin").startswith("150"))
        data.sendall(os.urandom(MIB))
        raw.sock.sendall(b"ABOR\r\n")
        first, second = raw.reply(), raw.reply()
        data.close()
        check("ABOR during an upload answers 426 then 226", first.startswith("426") and second.startswith("226"))
        check("an aborted overwrite leaves the old file as it was", (served / "keep.bin").read_bytes() == original and not leftovers(served))
        replacement = os.urandom(2 * MIB + 3)
        ftp.storbinary("STOR /HDD/keep.bin", io.BytesIO(replacement))
        check("a finished overwrite replaces the file and leaves no temporary files",
              (served / "keep.bin").read_bytes() == replacement and not leftovers(served))

        # Finding 3: a client that cancels by hanging up does not leave a file that looks complete.
        raw.command("NOOP")
        _, data_port = raw.passive()
        data = socket.create_connection(("127.0.0.1", data_port), timeout=20)
        raw.command("STOR /HDD/cancelled.bin")
        data.sendall(os.urandom(MIB // 2))
        raw.hang_up()
        data.close()
        raw = Raw(port)
        _, data_port = raw.passive()
        data = socket.create_connection(("127.0.0.1", data_port), timeout=20)
        raw.command("STOR /HDD/keep.bin")
        data.sendall(os.urandom(MIB // 2))
        raw.hang_up()
        data.close()
        for _ in range(50):
            if not (served / "cancelled.bin").exists():
                break
            threading.Event().wait(0.1)
        threading.Event().wait(0.5)
        check("an upload cancelled by closing the connections is not kept",
              not (served / "cancelled.bin").exists() and (served / "keep.bin").read_bytes() == replacement and not leftovers(served))
        stream = ftp.transfercmd("RETR /NULL/1024MB.bin")
        stream.recv(65536)
        first = ftp.abort()
        second = ftp.getresp()
        stream.close()
        check("ABOR during a download stops it with 426, then 226", first.startswith("426") and second.startswith("226"))

        # Finding 4: only the client's own address may open the data connection.
        raw = Raw(port)
        _, data_port = raw.passive()
        intruder = socket.socket()
        intruder.bind(("127.0.0.2", 0))
        intruder.connect(("127.0.0.1", data_port))
        intruder.settimeout(20)
        raw.sock.sendall(b"LIST /HDD\r\n")
        check("LIST starts", raw.reply().startswith("150"))
        try:
            stolen = intruder.recv(4096)
        except ConnectionError:
            stolen = b""
        data = socket.create_connection(("127.0.0.1", data_port), timeout=20)
        listing = bytearray()
        while True:
            chunk = data.recv(65536)
            if not chunk:
                break
            listing.extend(chunk)
        check("a data connection from another address is refused; the client's own gets the listing",
              stolen == b"" and b"keep.bin" in listing and raw.reply().startswith("226"))
        intruder.close()
        data.close()
        raw.command("QUIT")
        ftp.delete("/HDD/keep.bin")
        ftp.quit()
        host.kill()
        output = host.communicate(timeout=10)[0]
        check("the log names the refused address", "Refused a data connection from 127.0.0.2" in output)

    def login_file(text):
        path = Path(temp) / "FastFTP360.ini"
        path.write_bytes(text.encode("latin-1"))
        out = subprocess.run([str(HOST), "--login-file", str(path)],
                             capture_output=True, text=True, timeout=10).stdout.split()
        return out[1:]

    check("no settings file keeps xboxftp/xboxftp", login_file("") == ["on", "xboxftp", "xboxftp", "roundtrip"])
    check("the settings file sets user, password and login",
          login_file("login=off\r\nuser=marcus\r\npassword=S3cret!=#\r\n") == ["off", "marcus", "S3cret!=#", "roundtrip"])
    check("invalid settings are ignored",
          login_file("user=has space\npassword=\nlogin=maybe\nuser=" + "x" * 24 + "\n") == ["on", "xboxftp", "xboxftp", "roundtrip"])

print("%d FTP checks passed. Console execution remains untested." % passed)
