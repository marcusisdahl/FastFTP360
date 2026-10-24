"""Checks a running FastFTP360 server on a real console from a PC, with Python's ftplib.

Start the FTP server on the Xbox (A on the FTP tab), then:
  python tests/ftp_console.py 192.168.1.50
  python tests/ftp_console.py 192.168.1.50 --user marcus --password secret --root Usb0 --speed-mib 2048

Everything happens in a FastFTP360-test folder on the chosen drive, which is
deleted at the end. Also runs against tests/ftp_host on loopback (--root HDD --log "").
"""
import argparse
import ftplib
import hashlib
import io
import os
import socket
import sys
import time

MIB = 1048576
parser = argparse.ArgumentParser()
parser.add_argument("host")
parser.add_argument("--port", type=int, default=21)
parser.add_argument("--user", default="xboxftp")
parser.add_argument("--password", default="xboxftp")
parser.add_argument("--root", default="Hdd1", help="drive folder to test on, as listed at the top level")
parser.add_argument("--speed-mib", type=int, default=1024, help="size of the timed upload and download; 0 skips them")
parser.add_argument("--log", default="/Game/FastFTP360.log", help="log file to print the end of; empty skips it")
args = parser.parse_args()

folder = "/%s/FastFTP360-test" % args.root
passed = failed = 0


def check(name, condition, detail=""):
    global passed, failed
    if condition:
        passed += 1
        print("PASS " + name, flush=True)
    else:
        failed += 1
        print("FAIL " + name + (": " + detail if detail else ""), flush=True)


def login():
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=60)
    ftp.login(args.user, args.password)
    return ftp


def error_of(call):
    try:
        call()
        return ""
    except ftplib.all_errors as error:
        return str(error)


class Raw:
    """A control connection driven by hand, for cancels that ftplib cannot do."""

    def __init__(self):
        self.sock = socket.create_connection((args.host, args.port), timeout=60)
        self.file = self.sock.makefile("rb")
        self.reply()
        self.command("USER " + args.user)
        self.command("PASS " + args.password)

    def reply(self):
        line = self.file.readline().decode("latin-1").rstrip("\r\n")
        while len(line) > 3 and line[3] == "-":
            line = self.file.readline().decode("latin-1").rstrip("\r\n")
        return line

    def command(self, text):
        self.sock.sendall(text.encode("latin-1") + b"\r\n")
        return self.reply()

    def data(self):
        reply = self.command("PASV")
        numbers = reply[reply.index("(") + 1:reply.index(")")].split(",")
        return socket.create_connection((args.host, int(numbers[4]) * 256 + int(numbers[5])), timeout=60)

    def hang_up(self):
        self.file.close()
        self.sock.close()


def digest(ftp, path):
    h = hashlib.sha256()
    ftp.retrbinary("RETR " + path, h.update, blocksize=MIB)
    return h.hexdigest()


def names(ftp):
    return [n.rsplit("/", 1)[-1] for n in ftp.nlst(folder)]


def wait_until(condition, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if condition():
            return True
        time.sleep(0.25)
    return condition()


class Meter:
    """Prints a line every 64 MiB, so a console that stops shows where."""

    def __init__(self, label):
        self.label, self.bytes, self.begin, self.next = label, 0, time.time(), 64 * MIB

    def add(self, count):
        self.bytes += count
        while self.bytes >= self.next:
            seconds = time.time() - self.begin
            print("  %s %5d MiB at %6.1f s  %.2f MiB/s" % (self.label, self.next // MIB, seconds, self.next / MIB / seconds), flush=True)
            self.next += 64 * MIB

    def stopped(self, error):
        print("%s STOPPED after %d bytes (%.1f MiB) at %.1f s: %s" % (self.label, self.bytes, self.bytes / MIB, time.time() - self.begin, error), flush=True)


class Repeat(io.RawIOBase):
    """count bytes made of one block repeated, without holding them all."""

    def __init__(self, block, count, meter=None):
        self.block, self.left, self.at, self.meter = block, count, 0, meter

    def readable(self):
        return True

    def readinto(self, buffer):
        n = min(len(buffer), self.left, len(self.block) - self.at)
        buffer[:n] = self.block[self.at:self.at + n]
        self.at = (self.at + n) % len(self.block)
        self.left -= n
        if self.meter:
            self.meter.add(n)
        return n


ftp = login()
print(ftp.getwelcome())
reply = ftp.sendcmd("PASV")
address = socket.gethostbyname(args.host).replace(".", ",")
check("PASV announces the address the PC connected to", "(%s," % address in reply, reply)
if args.root not in ftp.nlst("/"):
    sys.exit("No /%s folder on this server. Pick one with --root: %s" % (args.root, " ".join(ftp.nlst("/"))))
if "FastFTP360-test" not in [n.rsplit("/", 1)[-1] for n in ftp.nlst("/" + args.root)]:
    ftp.mkd(folder)
for name in names(ftp):
    ftp.delete(folder + "/" + name)

# Finding 1: long names and paths get an error reply; the server stays up.
answer = error_of(lambda: ftp.sendcmd("SIZE %s/%s" % (folder, "a" * 300)))
check("a 300-character name gets 550 and the server stays up", answer.startswith("550") and ftp.voidcmd("NOOP").startswith("200"), answer)
answer = error_of(lambda: ftp.sendcmd("RNFR %s/%sx" % (folder, "e/" * 280)))
check("a 560-character rename path gets 550", answer.startswith("550") and ftp.voidcmd("NOOP").startswith("200"), answer)
answer = error_of(lambda: ftp.sendcmd("NOOP " + "x" * 700))
check("an over-long command line gets 500", answer.startswith("500") and ftp.voidcmd("NOOP").startswith("200"), answer)
answer = error_of(lambda: ftp.storbinary("STOR %s/%s" % (folder, "b" * 300), io.BytesIO(b"x")))
check("STOR with a 300-character name says it is too long", answer.startswith("553") and "too long" in answer, answer)

# Uploads and downloads stay exact.
original = os.urandom(64 * MIB)
ftp.storbinary("STOR %s/keep.bin" % folder, io.BytesIO(original), blocksize=MIB)
check("a 64 MiB upload downloads back byte for byte", digest(ftp, folder + "/keep.bin") == hashlib.sha256(original).hexdigest())

# Finding 2 and 3: cancelled overwrites keep the old file.
def cancelled_overwrite(how):
    raw = Raw()
    data = raw.data()
    started = raw.command("STOR %s/keep.bin" % folder)
    data.sendall(os.urandom(16 * MIB))
    if how == "abor":
        raw.sock.sendall(b"ABOR\r\n")
        first, second = raw.reply(), raw.reply()
        data.close()
        raw.command("QUIT")
        raw.hang_up()
        return started, first, second
    raw.hang_up()
    data.close()
    return started, "", ""


started, first, second = cancelled_overwrite("abor")
check("ABOR during an overwrite answers 426 then 226", started.startswith("150") and first.startswith("426") and second.startswith("226"),
      " / ".join([started, first, second]))
check("after ABOR the old file is unchanged and no temporary file is left",
      digest(ftp, folder + "/keep.bin") == hashlib.sha256(original).hexdigest() and names(ftp) == ["keep.bin"], str(names(ftp)))
cancelled_overwrite("hang up")
time.sleep(2)
check("after hanging up mid-overwrite the old file is unchanged and no temporary file is left",
      wait_until(lambda: names(ftp) == ["keep.bin"]) and digest(ftp, folder + "/keep.bin") == hashlib.sha256(original).hexdigest(),
      str(names(ftp)))
raw = Raw()
data = raw.data()
raw.command("STOR %s/new.bin" % folder)
data.sendall(os.urandom(16 * MIB))
raw.hang_up()
data.close()
check("a new upload cancelled by hanging up is deleted", wait_until(lambda: "new.bin" not in names(ftp)), str(names(ftp)))
replacement = os.urandom(32 * MIB + 5)
ftp.storbinary("STOR %s/keep.bin" % folder, io.BytesIO(replacement), blocksize=MIB)
check("a finished overwrite replaces the file", digest(ftp, folder + "/keep.bin") == hashlib.sha256(replacement).hexdigest() and
      names(ftp) == ["keep.bin"], str(names(ftp)))
answer = error_of(lambda: ftp.storbinary("STOR %s/%s" % (folder, "f" * 50), io.BytesIO(b"x")))
if answer:
    check("a 50-character name gets 553 with FATX's 42-character limit", answer.startswith("553") and "42" in answer, answer)
else:
    print("INFO this drive accepts 50-character names, so it is not FATX")
    ftp.delete("%s/%s" % (folder, "f" * 50))

# ABOR during a download.
stream = ftp.transfercmd("RETR %s/keep.bin" % folder)
stream.recv(65536)
first = ftp.abort()
second = error_of(ftp.voidresp) or "226"
stream.close()
check("ABOR during a download answers 426 then 226", first.startswith("426") and second.startswith("226"), first + " / " + second)
check("the session still works after the aborts", ftp.voidcmd("NOOP").startswith("200"))

# Speed: one large upload and download, for comparison with ~27.5 MiB/s (UE306) before the changes.
if args.speed_mib:
    block = os.urandom(64 * MIB)
    meter = Meter("UPLOAD  ")
    begin = time.time()
    try:
        reply = ftp.storbinary("STOR %s/speed.bin" % folder, Repeat(block, args.speed_mib * MIB, meter), blocksize=MIB)
    except (ftplib.all_errors, OSError) as error:
        meter.stopped(error)
        raise
    seconds = time.time() - begin
    print("UPLOAD   %d MiB in %.1f s: %.2f MiB/s (PC clock); server: %s" % (args.speed_mib, seconds, args.speed_mib / seconds, reply))
    size = ftp.size("%s/speed.bin" % folder)
    check("the timed upload has the right size", size == args.speed_mib * MIB, str(size))
    meter = Meter("DOWNLOAD")
    begin = time.time()
    try:
        reply = ftp.retrbinary("RETR %s/speed.bin" % folder, lambda chunk: meter.add(len(chunk)), blocksize=MIB)
    except (ftplib.all_errors, OSError) as error:
        meter.stopped(error)
        raise
    seconds = time.time() - begin
    print("DOWNLOAD %d MiB in %.1f s: %.2f MiB/s (PC clock); server: %s" % (meter.bytes // MIB, seconds, meter.bytes / MIB / seconds, reply))
    ftp.delete("%s/speed.bin" % folder)

for name in names(ftp):
    ftp.delete(folder + "/" + name)
ftp.rmd(folder)
check("the test folder is removed", "FastFTP360-test" not in [n.rsplit("/", 1)[-1] for n in ftp.nlst("/" + args.root)])
if args.log:
    lines = []
    ftp.retrbinary("RETR " + args.log, lambda chunk: lines.append(chunk))
    print("--- last 30 lines of %s ---" % args.log)
    print("\n".join(b"".join(lines).decode("latin-1").splitlines()[-30:]))
ftp.quit()
print("%d passed, %d failed." % (passed, failed))
sys.exit(1 if failed else 0)
