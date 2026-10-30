#!/usr/bin/env python3
"""Builds the PR report comment (see .github/workflows/pr-report.yml) for the commit $SHA and posts or updates it.
Everything read from the test logs is untrusted (the PR's code produced it): only lines matching the expected
formats are used, and every value is reduced to safe characters before it goes into the comment."""
import io, json, os, re, subprocess, sys, urllib.request, zipfile

REPO, SHA = os.environ["REPO"], os.environ["SHA"]
TOKEN = os.environ["GH_TOKEN"]
MARK = "<!-- vikey-pr-report -->"
WORKFLOWS = ["Engine rules", "windows", "linux", "android"]
API = "https://api.github.com"

def api(path, method="GET", body=None, raw=False):
    req = urllib.request.Request(path if path.startswith("http") else API + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": f"Bearer {TOKEN}", "Accept": "application/vnd.github+json",
                                          "X-GitHub-Api-Version": "2022-11-28"})
    with urllib.request.urlopen(req) as r:
        data = r.read()
    return data if raw else (json.loads(data) if data else None)

def safe(s, n=120):
    """Untrusted text → plain characters only (no markdown, HTML, mentions or links)."""
    s = re.sub(r"[^\w .,:;%/()+\-=>→→#'\"\[\]]", "", str(s), flags=re.UNICODE)
    return s.replace("@", "").strip()[:n]

# --- the pull request for this commit (from the API, never from an artifact)
prs = [p for page in (1, 2, 3) for p in api(f"/repos/{REPO}/pulls?state=open&per_page=100&page={page}")]
pr = next((p for p in prs if p["head"]["sha"] == SHA), None)
if not pr:
    print("no open PR at", SHA); sys.exit(0)
num = pr["number"]

# --- the latest run of each workflow for this commit
runs = api(f"/repos/{REPO}/actions/runs?head_sha={SHA}&event=pull_request&per_page=50")["workflow_runs"]
latest = {}
for r in runs:
    if r["name"] in WORKFLOWS and (r["name"] not in latest or r["run_number"] > latest[r["name"]]["run_number"]):
        latest[r["name"]] = r

ICON = {"success": "✅", "failure": "❌", "cancelled": "⚪", "skipped": "⚪", "timed_out": "❌", "action_required": "⏸️"}
def result(job):
    if job["status"] != "completed": return "⏳ running"
    return f'{ICON.get(job["conclusion"], "⚪")} {job["conclusion"]}'

def artifact(run, name):
    for a in api(f'/repos/{REPO}/actions/runs/{run["id"]}/artifacts')["artifacts"]:
        if a["name"] == name and not a["expired"]:
            z = zipfile.ZipFile(io.BytesIO(api(a["archive_download_url"], raw=True)))
            return z.read(z.namelist()[0]).decode("utf-8", "replace")[:400_000]
    return None

# --- CI results table
rows = []
for w in WORKFLOWS:
    r = latest.get(w)
    if not r:
        rows.append(f"| {w} | ⚪ not run (no files for it changed) | |"); continue
    if r["conclusion"] == "action_required":
        rows.append(f'| {w} | ⏸️ waiting for a maintainer to approve the run | [run]({r["html_url"]}) |'); continue
    for j in api(f'/repos/{REPO}/actions/runs/{r["id"]}/jobs')["jobs"]:
        rows.append(f'| {w} · {safe(j["name"], 60)} | {result(j)} | [log]({j["html_url"]}) |')

# --- engine metrics, from the check job's log
eng = []
er = latest.get("Engine rules")
log = artifact(er, "check-log") if er and er["status"] == "completed" else None
if log:
    def line(pat):
        m = re.search(pat, log, re.M); return safe(m.group(0), 200) if m else None
    m = re.search(r"^rules: (\d+)/(\d+) pass", log, re.M)
    if m: eng.append(f"| Rule cases (DECISIONS.md) | {m.group(1)}/{m.group(2)} pass |")
    fails = re.findall(r"^FAIL[^\n]*", log, re.M)
    for k in ("chat", "chat_typos"):
        m = re.search(rf"^metrics {k}: manual switches (\d+) -> (\d+), wrong words (\d+) -> (\d+)", log, re.M)
        if m:
            a, b, c, d = map(int, m.groups())
            arrow = lambda x, y: f"{x} → **{y}**" if x != y else f"{y}"
            eng.append(f"| Mixed-language corpus `{k}`: manual switches · wrong words (was → now) | {arrow(a, b)} · {arrow(c, d)} |")
    for label, pat in [("Undo after fix / switch / click", r"^undo after fix[^\n]*"), ("Driver output == simulator", r"^driver == simulator[^\n]*"),
                       ("Speed per key", r"^performance[^\n]*"), ("Realistic typing (3 seeds)", r"^realistic typing[^\n]*"),
                       ("Fat-finger model", r"^fat-finger[^\n]*"), ("Auto-correct", r"^auto-correct[^\n]*"),
                       ("Real-data auto-correct", r"^real-data auto-correct[^\n]*")]:
        v = line(pat)
        if v: eng.append(f"| {label} | {v.split(':', 1)[-1].strip() if ':' in v else v} |")
    golden = len(re.findall(r"^  [^\n]*: before ", log, re.M))
    if golden: eng.append(f"| Golden snapshot changes | {golden} |")
    if fails: eng.append("| Failures | " + "<br>".join(safe(f, 160) for f in fails[:8]) + " |")
elif er and er["status"] != "completed":
    eng.append("| Engine checks | ⏳ running |")

# --- iOS tests, from the ios job's summary
ios = []
ilog = artifact(er, "ios-log") if er and er["status"] == "completed" else None
if ilog:
    m = re.findall(r"Executed (\d+) tests?, with (\d+) failures?", ilog)
    if m: ios.append(f"Executed {m[-1][0]} tests, {m[-1][1]} failed.")
    failed = sorted(set(re.findall(r"Test Case '-\[([\w.]+ [\w]+)\]' failed", ilog)))
    if failed: ios.append("Failed: " + ", ".join(f"`{safe(t, 80)}`" for t in failed[:15]))

body = [MARK, f"### Vikey CI report · `{SHA[:7]}`", "", "| Check | Result | |", "|---|---|---|", *rows]
if eng: body += ["", "**Typing engine: tests and simulation metrics**", "", "| Metric | This PR |", "|---|---|", *eng]
if ios: body += ["", "**iOS keyboard tests:** " + " ".join(ios)]
body += ["", "<sub>Updated automatically when a check finishes. Numbers come from this PR's own CI run; the "
         "Windows/Linux/Android live typing tests run in VMs, not here. Desktop/Android jobs build only.</sub>"]
text = "\n".join(body)

# --- post or update the one report comment
mine = [c for page in (1, 2, 3) for c in api(f"/repos/{REPO}/issues/{num}/comments?per_page=100&page={page}")
        if c["user"]["login"] == "github-actions[bot]" and MARK in (c["body"] or "")]
if mine: api(f'/repos/{REPO}/issues/comments/{mine[0]["id"]}', "PATCH", {"body": text}); print("updated report on #", num)
else: api(f"/repos/{REPO}/issues/{num}/comments", "POST", {"body": text}); print("posted report on #", num)
