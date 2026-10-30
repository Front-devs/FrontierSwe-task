#!/usr/bin/env python3
"""rustLabs sample checker for Harbor / FrontierSWE-style task folders.

    python3 frontierswe-check.py path/to/your-task

Static checks only: nothing is built, run or uploaded, and no network is used. It catches the
small problems that get a task rejected before anyone looks at the idea: missing files, the
reference solution or hidden tests visible to the agent, an agent budget that is too short, leaked
keys, paths from your own machine, files too large to share.

FAIL lines must be fixed. WARN lines are worth fixing. When nothing fails, the last line reads

    FSWE-CHECK v1 PASS <task-name> <fingerprint>

Paste that line into the form. The fingerprint covers every file in the folder, so run the check
again after any change. Then run Harbor's own checks (https://github.com/harbor-framework/harbor):

    harbor run -p path/to/your-task -a oracle   # the reference solution should score full marks
    harbor run -p path/to/your-task -a nop      # doing nothing should score 0

Needs Python 3.11 or newer (for tomllib).
"""
import hashlib
import os
import re
import sys

VERSION = "v1"
SKIP_DIRS = {".git", "__pycache__", "node_modules", ".venv", "venv", ".mypy_cache", ".pytest_cache", ".idea", ".vscode"}
SKIP_FILES = {".DS_Store", "Thumbs.db"}
MIN_AGENT_SECONDS = 2 * 3600       # below this it is not a long-horizon task
GOOD_AGENT_SECONDS = 10 * 3600     # FrontierSWE tasks give the agent 20 hours
MAX_FILE_BYTES = 100 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
BIG_FILE_BYTES = 20 * 1024 * 1024
SCAN_BYTES = 2 * 1024 * 1024

# Live-provider credential formats. A key made up for a security task should not match these.
SECRET_PATTERNS = [
    ("AWS access key", re.compile(rb"\b(AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("GitHub token", re.compile(rb"\b(ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{36}\b|\bgithub_pat_[A-Za-z0-9_]{60,}")),
    ("Anthropic key", re.compile(rb"\bsk-ant-[A-Za-z0-9_-]{20,}")),
    ("OpenAI key", re.compile(rb"\bsk-(proj-)?[A-Za-z0-9]{40,}")),
    ("Slack token", re.compile(rb"\bxox[abprs]-[A-Za-z0-9-]{10,}")),
    ("Google API key", re.compile(rb"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("Stripe live key", re.compile(rb"\b(sk|rk)_live_[A-Za-z0-9]{20,}")),
    ("Hugging Face token", re.compile(rb"\bhf_[A-Za-z0-9]{34,}\b")),
]
PRIVATE_KEY = re.compile(rb"-----BEGIN (RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----")
HOST_PATH = re.compile(rb"(?<![\w/])(/Users/[A-Za-z0-9._-]+|/home/(?!agent\b|user\b|ubuntu\b|runner\b)[A-Za-z0-9._-]+|C:\\\\Users\\\\)")
PLACEHOLDER = re.compile(r"\b(TODO|FIXME|TBD|lorem ipsum)\b|<\s*(fill|insert|your)[^>]*>", re.I)
TEXT_EXT = {".md", ".txt", ".toml", ".sh", ".py", ".json", ".yaml", ".yml", ".cfg", ".ini", ".env", ".js", ".ts", ".go",
            ".rs", ".c", ".h", ".cc", ".cpp", ".java", ".rb", ".php", ".sql", ".zig", ".swift", ".lua", ""}

fails, warns = [], []


def fail(msg):
    fails.append(msg)


def warn(msg):
    warns.append(msg)


def walk(root):
    for dirpath, dirnames, filenames in os.walk(root):
        junk = [d for d in dirnames if d in SKIP_DIRS]
        for d in junk:
            warn(f"{os.path.relpath(os.path.join(dirpath, d), root)}/ is in the folder; leave it out of what you send")
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for f in sorted(filenames):
            if f in SKIP_FILES:
                continue
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                warn(f"{os.path.relpath(p, root)} is a symlink; replace it with the real file")
                continue
            yield os.path.relpath(p, root).replace(os.sep, "/"), p


def read(p, limit=None):
    with open(p, "rb") as fh:
        return fh.read(limit) if limit else fh.read()


def load_toml(p):
    try:
        import tomllib
    except ImportError:
        sys.exit("This checker needs Python 3.11 or newer (tomllib).")
    try:
        with open(p, "rb") as fh:
            return tomllib.load(fh)
    except Exception as e:  # noqa: BLE001 - any parse error is the finding
        fail(f"task.toml does not parse: {e}")
        return None


def number(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def check_toml(cfg):
    task = cfg.get("task") or {}
    if not task.get("name"):
        fail("task.toml: [task] name is missing")
    if not task.get("description"):
        warn("task.toml: [task] description is missing")
    agent = cfg.get("agent") or {}
    t = number(agent.get("timeout_sec"))
    if t is None:
        fail("task.toml: [agent] timeout_sec is missing (FrontierSWE tasks use 72000, i.e. 20 hours)")
    elif t < MIN_AGENT_SECONDS:
        fail(f"task.toml: [agent] timeout_sec is {t:g}s; a long-horizon task needs at least {MIN_AGENT_SECONDS}s")
    elif t < GOOD_AGENT_SECONDS:
        warn(f"task.toml: [agent] timeout_sec is {t:g}s; FrontierSWE tasks give the agent 72000s")
    if number((cfg.get("verifier") or {}).get("timeout_sec")) is None:
        fail("task.toml: [verifier] timeout_sec is missing")
    env = cfg.get("environment") or {}
    for k in ("cpus", "memory_mb"):
        if number(env.get(k)) is None:
            warn(f"task.toml: [environment] {k} is not set")
    if not (cfg.get("metadata") or {}).get("difficulty"):
        warn("task.toml: [metadata] difficulty is not set")
    return agent


def dockerfile_lines(text):
    """Logical Dockerfile instructions, with continuations joined and comments dropped."""
    out, cur = [], ""
    for raw in text.splitlines():
        line = raw.strip()
        if not cur and (not line or line.startswith("#")):
            continue
        if line.endswith("\\"):
            cur += line[:-1] + " "
            continue
        out.append((cur + line).strip())
        cur = ""
    if cur:
        out.append(cur.strip())
    return out


def check_dockerfile(task_dir, env_dir, agent):
    path = os.path.join(env_dir, "Dockerfile")
    text = read(path).decode("utf-8", "replace")
    lines = dockerfile_lines(text)
    froms = [l for l in lines if l.upper().startswith("FROM ")]
    if not froms:
        fail("environment/Dockerfile has no FROM line")
    stages = set()
    for l in froms:
        parts = l.split()
        image = next((p for p in parts[1:] if not p.startswith("--")), "")
        if len(parts) >= 4 and parts[-2].upper() == "AS":
            stages.add(parts[-1].lower())
        if image.lower() in stages or image.lower() == "scratch" or "$" in image:
            continue
        if "@sha256:" not in image and (":" not in image.split("/")[-1] or image.endswith(":latest")):
            warn(f"Dockerfile: pin the base image to a version or digest (FROM {image})")
    agent_user = str(agent.get("user") or "")
    tests_in_image = False
    for l in lines:
        op = l.split(None, 1)[0].upper()
        if op not in ("COPY", "ADD"):
            continue
        args = [a for a in l.split()[1:] if not a.startswith("--")]
        if any(a.startswith("--from") for a in l.split()[1:]) or len(args) < 2:
            continue
        if args[0].startswith("["):
            continue  # JSON form; skip rather than misparse
        *srcs, dest = args
        for s in srcs:
            if re.match(r"https?://", s):
                warn(f"Dockerfile: {op} downloads {s}; pin it with a checksum")
                continue
            norm = s.lstrip("./")
            if norm.split("/")[0] in ("solution", "solutions", "oracle") or "solve.sh" in norm:
                fail(f"Dockerfile copies the reference solution into the image ({op} {s}); the agent could read it")
                continue
            if norm.split("/")[0] in ("tests", "test") or norm.startswith("tests/"):
                tests_in_image = True
                if re.match(r"^/(app|workspace|home|tmp|srv|opt/app)(/|$)", dest):
                    fail(f"Dockerfile copies the hidden tests to {dest}, where the agent can read them; use a root-only path such as /root/tests")
            if any(ch in s for ch in "*?$"):
                continue
            if not os.path.exists(os.path.join(env_dir, s)):
                fail(f"Dockerfile: {op} source {s} does not exist in environment/")
    if tests_in_image and agent_user in ("", "root", "0"):
        fail("The hidden tests are baked into the image but [agent] user is root (or unset), so the agent can read them; set a non-root [agent] user")
    for l in lines:
        if re.search(r"\bpip3? install\b", l) and not re.search(r"-r\s|--requirement|==|@\s*https?://|\.whl\b", l):
            pkgs = [w for w in re.split(r"\s+", l.split("install", 1)[1]) if w and not w.startswith("-") and w not in ("&&", ";", "\\")]
            if pkgs:
                warn(f"Dockerfile: pin pip versions (pkg==x.y) in: {l[:120]}")
                break
    if not any(l.upper().startswith("WORKDIR ") for l in lines):
        warn("Dockerfile: no WORKDIR; set where the agent starts")


def check_tests(task_dir, tests_dir):
    test_sh = os.path.join(tests_dir, "test.sh")
    body = read(test_sh).decode("utf-8", "replace")
    if not body.strip():
        fail(f"{os.path.relpath(test_sh, task_dir)} is empty")
        return
    if not body.startswith("#!"):
        warn(f"{os.path.relpath(test_sh, task_dir)} has no #! line")
    writes_reward = False
    for _, p in walk(tests_dir):
        if os.path.getsize(p) <= SCAN_BYTES and re.search(rb"reward\.(txt|json)", read(p, SCAN_BYTES)):
            writes_reward = True
            break
    if not writes_reward:
        fail("The verifier never writes /logs/verifier/reward.txt or reward.json, so Harbor cannot score the task")
    for ref in re.findall(r"(?:/tests/|\$TEST_DIR/|\./)([A-Za-z0-9_./-]+\.(?:py|sh|json|txt))", body):
        if not os.path.exists(os.path.join(tests_dir, ref)):
            warn(f"test.sh mentions {ref}, which is not in {os.path.relpath(tests_dir, task_dir)}/")


def check_instruction(task_dir):
    text = read(os.path.join(task_dir, "instruction.md")).decode("utf-8", "replace")
    if len("".join(text.split())) < 200:
        fail("instruction.md is too short to specify a long-horizon task (under 200 characters)")
    if re.search(r"solve\.sh|/solution\b|/oracle\b|/root/tests|/logs/verifier", text):
        fail("instruction.md points at the solution, the hidden tests or the verifier output")
    m = PLACEHOLDER.search(text)
    if m:
        fail(f"instruction.md still has placeholder text: {m.group(0)!r}")


def authored(rel):
    """Files the task author wrote, as opposed to upstream source vendored into environment/."""
    top = rel.split("/")[0]
    if "/" not in rel or top in ("solution", "tests", "preflight"):
        return True
    parts = rel.split("/")
    return top == "environment" and (len(parts) == 2 or parts[1] in ("tests", "setup"))


def scan_files(task_dir):
    total = 0
    digest = hashlib.sha256()
    for rel, p in walk(task_dir):
        size = os.path.getsize(p)
        total += size
        if size > MAX_FILE_BYTES:
            fail(f"{rel} is {size // 2**20} MB; fetch large data in the Docker build instead of shipping it")
        elif size > BIG_FILE_BYTES:
            warn(f"{rel} is {size // 2**20} MB")
        name = rel.rsplit("/", 1)[-1]
        if name == ".env" or name.startswith(".env."):
            warn(f"{rel}: make sure it holds no real credentials")
        digest.update(rel.encode() + b"\0")
        with open(p, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                digest.update(chunk)
        digest.update(b"\0")
        ext = os.path.splitext(name)[1].lower()
        if size > SCAN_BYTES or ext not in TEXT_EXT:
            continue
        data = read(p)
        if b"\0" in data[:4096]:
            continue
        for label, pat in SECRET_PATTERNS:
            if pat.search(data):
                fail(f"{rel} contains what looks like a real {label}; remove it (made-up keys must not use a live format)")
        if PRIVATE_KEY.search(data):
            warn(f"{rel} contains a private key; fine if you generated it for the task, never a real one")
        m = HOST_PATH.search(data) if authored(rel) else None
        if m:
            fail(f"{rel} uses a path from your own machine ({m.group(0).decode(errors='replace')}); use paths inside the container")
    if total > MAX_TOTAL_BYTES:
        fail(f"The folder is {total // 2**20} MB; keep it under {MAX_TOTAL_BYTES // 2**20} MB")
    return digest.hexdigest()[:12]


def main():
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    task_dir = os.path.abspath(sys.argv[1])
    if not os.path.isdir(task_dir):
        print(f"Not a folder: {sys.argv[1]}")
        return 2

    def exists(rel):
        return os.path.isfile(os.path.join(task_dir, rel))

    for rel in ("instruction.md", "task.toml", "environment/Dockerfile"):
        if not exists(rel):
            fail(f"{rel} is missing")
    tests_dir = next((os.path.join(task_dir, d) for d in ("tests", "environment/tests") if exists(f"{d}/test.sh")), None)
    if not tests_dir:
        fail("No verifier: add tests/test.sh (or environment/tests/test.sh) that writes /logs/verifier/reward.txt")
    if not exists("solution/solve.sh"):
        fail("solution/solve.sh is missing; samples need a runnable reference solution (harbor run -a oracle)")
    elif not read(os.path.join(task_dir, "solution/solve.sh")).strip():
        fail("solution/solve.sh is empty")

    cfg = load_toml(os.path.join(task_dir, "task.toml")) if exists("task.toml") else None
    agent = check_toml(cfg) if cfg is not None else {}
    if exists("environment/Dockerfile"):
        check_dockerfile(task_dir, os.path.join(task_dir, "environment"), agent)
    if tests_dir:
        check_tests(task_dir, tests_dir)
    if exists("instruction.md"):
        check_instruction(task_dir)
    fingerprint = scan_files(task_dir)

    name = str(((cfg or {}).get("task") or {}).get("name") or os.path.basename(task_dir))
    name = re.sub(r"[^A-Za-z0-9._/-]", "-", name)[:80] or "task"
    for w in dict.fromkeys(warns):
        print(f"WARN  {w}")
    for f in dict.fromkeys(fails):
        print(f"FAIL  {f}")
    print()
    if fails:
        print(f"FSWE-CHECK {VERSION} FAIL {name}: fix the FAIL lines above and run it again")
        return 1
    print(f"FSWE-CHECK {VERSION} PASS {name} {fingerprint}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
