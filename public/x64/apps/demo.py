# demo.py — runs inside CPython 3.12 (python.org Windows build) on the Nebula x64 emulator
import sys, platform, json, re, sqlite3, decimal, hashlib, zlib, datetime, dataclasses, threading, statistics, os

print(f"Python {sys.version.split()[0]} on {sys.platform} / {platform.machine()} — {platform.python_compiler()}")
print("executable:", sys.executable, "| cwd:", os.getcwd())

# standard library: regex, json, hashing, compression
text = "Nebula OS runs real Windows x64 binaries in the browser — 2026-10-30"
print("words:", len(re.findall(r"\w+", text)), "| date:", re.search(r"\d{4}-\d{2}-\d{2}", text).group())
print("json  :", json.dumps({"answer": 6 * 7, "pi": round(3.14159265, 3), "ok": True}, sort_keys=True))
print("sha256:", hashlib.sha256(text.encode()).hexdigest()[:32], "| zlib:", len(zlib.compress(text.encode() * 50)), "bytes")

# C extension modules (.pyd + sqlite3.dll loaded from the virtual disk)
con = sqlite3.connect(":memory:")
con.execute("create table t(name text, score real)")
con.executemany("insert into t values (?, ?)", [("alice", 91.5), ("bob", 78.0), ("carol", 85.25)])
print("sqlite:", con.execute("select count(*), round(avg(score), 2), max(name) from t").fetchone(), "| v" + sqlite3.sqlite_version)
decimal.getcontext().prec = 30
print("decimal:", decimal.Decimal(2).sqrt())

# dataclasses, statistics, datetime
@dataclasses.dataclass(order=True)
class Point:
    x: float
    y: float = 0.0
pts = sorted(Point(x, x * x) for x in (3, -1, 2))
print("dataclass:", pts[0], "| mean y:", statistics.mean(p.y for p in pts))
print("today:", datetime.date.today().isoformat(), "| weekday:", datetime.date.today().strftime("%A"))

# threads (Win32 CreateThread + critical sections underneath)
results = []
lock = threading.Lock()
def worker(n):
    s = sum(i * i for i in range(n))
    with lock:
        results.append((n, s))
threads = [threading.Thread(target=worker, args=(k,)) for k in (1000, 2000, 3000)]
for t in threads: t.start()
for t in threads: t.join()
print("threads:", sorted(results))

# exceptions with a traceback
try:
    json.loads("{broken")
except json.JSONDecodeError as e:
    print("caught:", type(e).__name__, "-", e.msg, "at", e.pos)

print("done — try: -i -q (REPL, type into the console), -m json.tool demo.json, -c \"print(2**100)\"")
