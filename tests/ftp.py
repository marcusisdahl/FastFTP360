"""FTP server tests on loopback with Python's ftplib; no Xbox required.

On Windows, build.ps1 -Tests builds build/host/ftp-host.exe and runs this. On
Linux or macOS, build the same server first:
  g++ -std=c++11 -O1 -include tests/ftp_posix.h tests/ftp_host.cpp shared/ftp.cpp shared/checksum.cpp shared/bench.cpp -o build/host/ftp-host -lpthread
"""
import contextlib
import hashlib
import ftplib
import io
import os
import socket
import subprocess
import tempfile
import threading
import time
import zlib
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
def server(folder, sessions=1, credentials=(), speed=0, download_speed=0, disk=0):
    port = free_port()
    limit = ((["--speed", str(speed)] if speed else []) + (["--download-speed", str(download_speed)] if download_speed else [])
             + (["--disk", str(disk)] if disk else []))
    process = subprocess.Popen([str(HOST), *limit, str(port), str(folder), str(sessions), *credentials],
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


def error_of(call):
    try:
        call()
        return ""
    except ftplib.all_errors as error:
        return str(error)


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
        lines = output.splitlines()
        starts = [line for line in lines if line.startswith("START ")]
        ends = [line for line in lines if line.startswith(("DONE ", "FAILED "))]
        check("every transfer is announced before data moves and ends once",
              len(starts) == len(ends) and lines.index("START STOR big.bin") < next(i for i, line in enumerate(lines) if line.startswith("DONE STOR big.bin")))

    with server(served, sessions=3, speed=8) as (port, host):
        ftp = login(port)
        begin = time.time()
        ftp.storbinary("STOR /NULL/limited.bin", io.BytesIO(bytes(16 * MIB)), blocksize=MIB)
        up = time.time() - begin
        begin = time.time()
        limited = bytearray()
        ftp.retrbinary("RETR /NULL/16MB.bin", limited.extend)
        down = time.time() - begin
        ftp.quit()
        check("an 8 MiB/s speed limit holds 16 MiB up and down to about 2 s each",
              1.9 <= up < 6 and 1.9 <= down < 6 and len(limited) == 16 * MIB)

        def limited_upload(index):
            client = login(port)
            client.storbinary("STOR /NULL/parallel%d.bin" % index, io.BytesIO(bytes(8 * MIB)), blocksize=MIB)
            client.quit()

        begin = time.time()
        uploads = [threading.Thread(target=limited_upload, args=(i,)) for i in range(2)]
        for upload in uploads:
            upload.start()
        for upload in uploads:
            upload.join(30)
        both = time.time() - begin
        output = host.communicate(timeout=10)[0]
        check("the limit holds two parallel uploads together, 8 MiB each in about 2 s", 1.9 <= both < 6 and output.count("DONE STOR parallel") == 2)
        check("the log says when a limited transfer starts", "Upload /NULL/limited.bin started, limited to 8 MiB/s" in output)

    with server(served, download_speed=8) as (port, host):
        ftp = login(port)
        begin = time.time()
        ftp.storbinary("STOR /NULL/fast.bin", io.BytesIO(bytes(16 * MIB)), blocksize=MIB)
        up = time.time() - begin
        begin = time.time()
        limited = bytearray()
        ftp.retrbinary("RETR /NULL/16MB.bin", limited.extend)
        down = time.time() - begin
        ftp.quit()
        lines = host.communicate(timeout=10)[0].splitlines()
        check("a downloads-only limit of 8 MiB/s holds a 16 MiB download to about 2 s and leaves uploads alone",
              up < 1.5 and 1.9 <= down < 6 and len(limited) == 16 * MIB)
        check("the log shows the limit on downloads only",
              any(line.endswith("  Upload /NULL/fast.bin started") for line in lines)
              and any(line.endswith("  Download /NULL/16MB.bin started, limited to 8 MiB/s") for line in lines))

    # Uncached disk I/O writes whole blocks and the shorter last one through the
    # cache. Windows, and the Linux shim, refuse unaligned uncached reads and writes.
    sizes = [0, 1, 4095, 65536, 262144, 262145, 3 * MIB + 123]
    with server(served, disk=262144) as (port, host):
        ftp = login(port)
        exact = True
        for size in sizes:
            ftp.storbinary("STOR /HDD/u%d.bin" % size, io.BytesIO(payload[:size]))
            back = bytearray()
            ftp.retrbinary("RETR /HDD/u%d.bin" % size, back.extend)
            exact = exact and (served / ("u%d.bin" % size)).read_bytes() == payload[:size] and bytes(back) == payload[:size]
        check("uncached 256 KiB blocks upload and download files of every size exactly", exact)
        ftp.storbinary("STOR /HDD/u1.bin", io.BytesIO(payload[:300000]))
        check("an uncached upload replaces an existing file",
              (served / "u1.bin").read_bytes() == payload[:300000] and not list(served.glob("FastFTP360-*")))
        for size in sizes:
            ftp.delete("/HDD/u%d.bin" % size)
        ftp.quit()
        check("no uncached transfer failed", "FAILED" not in host.communicate(timeout=10)[0])
    with server(served, disk=1048576) as (port, host):
        ftp = login(port)
        ftp.storbinary("STOR /HDD/u.bin", io.BytesIO(payload))
        back = bytearray()
        ftp.retrbinary("RETR /HDD/u.bin", back.extend)
        check("uncached 1 MiB blocks move 5 MiB exactly", (served / "u.bin").read_bytes() == payload and bytes(back) == payload)
        ftp.delete("/HDD/u.bin")
        ftp.quit()

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
        expected = ["Connected", "Logged in as marcus", "Upload /HDD/log.bin started", "Upload /HDD/log.bin, 5 bytes in", "Renamed /HDD/log.bin to /HDD/log2.bin",
                    "Deleted /HDD/log2.bin", "Disconnected"]
        check("the log records login, upload start and end, rename, delete and disconnect with the client address",
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
        # Short enough to create under a Windows temp folder; /HDD/<40>/<250> still passes the 259 limit.
        deep = "c" * 40
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
        threading.Event().wait(1)
        # A new file cut short stays for resuming (REST or APPE); it is logged as failed.
        cancelled = served / "cancelled.bin"
        check("an upload cancelled by closing the connections keeps its part for resuming, and an overwrite keeps the old file",
              cancelled.exists() and cancelled.stat().st_size <= MIB // 2 and (served / "keep.bin").read_bytes() == replacement
              and not leftovers(served))
        cancelled.unlink()
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

    # Resume, checksums and exact dates.
    for disk in (0, 262144):
        mode = "uncached" if disk else "cached"
        with server(served, sessions=0, disk=disk) as (port, host):
            ftp = login(port)
            features = ftp.sendcmd("FEAT")
            check(mode + ": FEAT lists REST STREAM, MDTM, MFMT, MLST and HASH",
                  all(f in features for f in ("REST STREAM", "MDTM", "MFMT", "MLST type*;size*;modify*;", "HASH CRC32;MD5;SHA-1*", "XCRC")))
            whole = os.urandom(3 * MIB + 777)
            # Uploads resumed at a block boundary and in the middle of one (cached then).
            for cut in (MIB, MIB + 4097):
                ftp.storbinary("STOR /HDD/resume.bin", io.BytesIO(whole[:cut]))
                ftp.storbinary("STOR /HDD/resume.bin", io.BytesIO(whole[cut:]), rest=cut)
                check(mode + ": STOR after REST %d continues the file" % cut, (served / "resume.bin").read_bytes() == whole)
            ftp.storbinary("STOR /HDD/resume.bin", io.BytesIO(whole[:2 * MIB] + b"junk" * 100))
            ftp.storbinary("STOR /HDD/resume.bin", io.BytesIO(whole[2 * MIB:]), rest=2 * MIB)
            check(mode + ": a resumed upload replaces anything past the restart point", (served / "resume.bin").read_bytes() == whole)
            (served / "resume.bin").write_bytes(whole[:5000])
            ftp.storbinary("APPE /HDD/resume.bin", io.BytesIO(whole[5000:]))
            check(mode + ": APPE adds to the end of the file", (served / "resume.bin").read_bytes() == whole)
            ftp.storbinary("APPE /HDD/fresh.bin", io.BytesIO(b"fresh"))
            check(mode + ": APPE to a new name creates it", (served / "fresh.bin").read_bytes() == b"fresh")
            ftp.delete("/HDD/fresh.bin")
            answered = error_of(lambda: ftp.storbinary("STOR /HDD/resume.bin", io.BytesIO(b"x"), rest=len(whole) + 1))
            check(mode + ": REST past the end of the file is refused", answered.startswith("551") and (served / "resume.bin").read_bytes() == whole)
            answered = error_of(lambda: ftp.storbinary("STOR /HDD/missing.bin", io.BytesIO(b"x"), rest=10))
            check(mode + ": REST on a missing file is refused", answered.startswith("551") and not (served / "missing.bin").exists())
            for start in (0, 1, 65536, 262144 + 3, len(whole) - 10, len(whole)):
                got = bytearray()
                ftp.retrbinary("RETR /HDD/resume.bin", got.extend, rest=start)
                check(mode + ": RETR after REST %d sends the rest of the file" % start, bytes(got) == whole[start:])
            answered = error_of(lambda: ftp.retrbinary("RETR /HDD/resume.bin", lambda b: None, rest=len(whole) + 1))
            check(mode + ": RETR past the end is refused", answered.startswith("551"))
            got = bytearray()
            ftp.retrbinary("RETR /NULL/1MB.bin", got.extend, rest=70000)
            check(mode + ": a resumed NULL download continues the pattern", bytes(got) == pattern(0x46545021, MIB)[70000:])
            ftp.retrbinary("RETR /HDD/resume.bin", lambda b: None)
            check(mode + ": REST applies to one transfer only", True)
            got = bytearray()
            ftp.retrbinary("RETR /HDD/resume.bin", got.extend)
            check(mode + ": the next RETR starts at 0 again", bytes(got) == whole)

            # A new upload cut short, then resumed from the size the server reports.
            raw = Raw(port)
            raw.command("TYPE I")
            _, data_port = raw.passive()
            data = socket.create_connection(("127.0.0.1", data_port), timeout=20)
            raw.command("STOR /HDD/broken.bin")
            data.sendall(whole[:2 * MIB + 5])
            threading.Event().wait(0.5)
            raw.hang_up()
            data.close()
            threading.Event().wait(1)
            kept = ftp.size("/HDD/broken.bin")
            ftp.storbinary("STOR /HDD/broken.bin", io.BytesIO(whole[kept:]), rest=kept)
            check(mode + ": a broken upload resumes from its kept size (%d bytes)" % kept, (served / "broken.bin").read_bytes() == whole)
            ftp.delete("/HDD/broken.bin")

            for algorithm, expected in (("SHA-1", hashlib.sha1(whole).hexdigest()), ("MD5", hashlib.md5(whole).hexdigest()),
                                        ("CRC32", "%08x" % zlib.crc32(whole))):
                check(mode + ": OPTS HASH " + algorithm, ftp.sendcmd("OPTS HASH " + algorithm) == "200 " + algorithm)
                check(mode + ": HASH gives the " + algorithm + " of the file",
                      ftp.sendcmd("HASH /HDD/resume.bin") == "213 %s 0-%d %s /HDD/resume.bin" % (algorithm, len(whole), expected))
            ftp.cwd("/HDD")
            check(mode + ": XCRC, XMD5 and XSHA1 give the same checksums",
                  ftp.sendcmd("XCRC /HDD/resume.bin") == "250 %08x" % zlib.crc32(whole)
                  and ftp.sendcmd('XMD5 "/HDD/resume.bin"') == "250 " + hashlib.md5(whole).hexdigest()
                  and ftp.sendcmd("XSHA1 resume.bin") == "250 " + hashlib.sha1(whole).hexdigest())
            check(mode + ": HASH of an empty file", ftp.storbinary("STOR /HDD/empty.bin", io.BytesIO(b"")) and
                  ftp.sendcmd("XSHA1 /HDD/empty.bin") == "250 " + hashlib.sha1(b"").hexdigest())
            ftp.delete("/HDD/empty.bin")
            check(mode + ": HASH of a folder or missing file is refused, and bad algorithms too",
                  error_of(lambda: ftp.sendcmd("HASH /HDD")).startswith("550") and error_of(lambda: ftp.sendcmd("HASH /HDD/nope")).startswith("550")
                  and error_of(lambda: ftp.sendcmd("OPTS HASH SHA-512")).startswith("501"))

            check(mode + ": MFMT sets the file's time", ftp.sendcmd("MFMT 20240229123456 /HDD/resume.bin") == "213 Modify=20240229123456; /HDD/resume.bin"
                  and int((served / "resume.bin").stat().st_mtime) == 1709210096)
            check(mode + ": MDTM reads it back", ftp.sendcmd("MDTM /HDD/resume.bin") == "213 20240229123456")
            check(mode + ": MFMT with fractional seconds and a bad time",
                  ftp.sendcmd("MFMT 20230101000000.123 /HDD/resume.bin").startswith("213 Modify=20230101000000;")
                  and error_of(lambda: ftp.sendcmd("MFMT 2023 /HDD/resume.bin")).startswith("501")
                  and error_of(lambda: ftp.sendcmd("MFMT 20231301000000 /HDD/resume.bin")).startswith("501")
                  and error_of(lambda: ftp.sendcmd("MDTM /HDD")).startswith("550"))
            ftp.mkd("/HDD/sub")
            facts = {}
            for name, entry in ftp.mlsd("/HDD"):
                facts[name] = entry
            check(mode + ": MLSD lists files with type, size and modify, and folders as type=dir",
                  facts.get("resume.bin") == {"type": "file", "size": str(len(whole)), "modify": "20230101000000"}
                  and facts.get("sub", {}).get("type") == "dir" and "modify" in facts["sub"])
            top = dict(ftp.mlsd("/"))
            check(mode + ": MLSD of the top level lists the drive folders", top.get("HDD") == {"type": "dir"} and top.get("NULL") == {"type": "dir"})
            check(mode + ": MLSD of a file is refused", error_of(lambda: list(ftp.mlsd("/HDD/resume.bin"))).startswith("501"))
            lines = ftp.sendcmd("MLST /HDD/resume.bin").splitlines()
            check(mode + ": MLST describes one file on the control connection",
                  lines[0].startswith("250-") and lines[1] == " type=file;size=%d;modify=20230101000000; /HDD/resume.bin" % len(whole)
                  and lines[2].startswith("250 "))
            check(mode + ": MLST of a drive folder and of nothing",
                  " type=dir; /HDD" in ftp.sendcmd("MLST /HDD") and error_of(lambda: ftp.sendcmd("MLST /HDD/nope")).startswith("550"))
            ftp.rmd("/HDD/sub")
            ftp.delete("/HDD/resume.bin")
            ftp.quit()

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

    def setting(text, key):
        path = Path(temp) / "FastFTP360.ini"
        path.write_bytes(text.encode("latin-1"))
        return subprocess.run([str(HOST), "--setting", str(path), key],
                              capture_output=True, text=True, timeout=10).stdout.strip()

    saved = "login=on\r\nuser=xboxftp\r\npassword=xboxftp\r\nbuffers=0\r\ncpu=auto\r\ndrawing=paused\r\nspeed=16\r\n"
    check("the Xbox settings lines are found, the last one wins and keys ignore case",
          setting(saved, "speed") == "SETTING [16]" and setting(saved, "CPU") == "SETTING [auto]"
          and setting("speed=12\nspeed=20", "speed") == "SETTING [20]" and setting("speed=", "speed") == "SETTING []")
    check("other keys, missing keys and values too long for the Xbox are not found",
          setting("speedy=12\nspeed 16\n", "speed") == "NO SETTING" and setting(saved, "disk") == "NO SETTING"
          and setting("speed=12\nspeed=" + "1" * 16, "speed") == "SETTING [12]")
    check("the login is still read from a file with settings lines", login_file(saved) == ["on", "xboxftp", "xboxftp", "roundtrip"])

print("%d FTP checks passed. Console execution remains untested." % passed)
