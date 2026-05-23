#!/usr/bin/env python3
"""
CoinDeck GitHub dashboard daemon — polls GitHub via `gh` CLI every 2 min,
pushes counts to ESP32 at coindeck.local:8080/github.

Pulls 5 search queries (well under GitHub's auth rate limit of 5000/hr):
  open PRs           — is:pr is:open author:@me
  pending reviews    — is:pr is:open review-requested:@me
  CI passed/failed/pending — same author scope, filtered by status:

Requires `gh` CLI installed and authenticated (`gh auth status`).
"""
import json
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ESP32_URL    = "http://coindeck.local:8080/github"
POLL_S       = 120
GH_TIMEOUT   = 15
IGNORE_FILE  = Path.home() / ".config" / "coindeck" / "ignored_prs.txt"

def log(m): print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)

def load_ignored():
    """Read ignored PR list. Each line: 'owner/repo#NN'. Comments start with '#' at column 0."""
    try:
        lines = IGNORE_FILE.read_text().splitlines()
    except OSError:
        return set()
    out = set()
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        out.add(s)
    return out

def item_key(item):
    """Build 'owner/repo#NN' key from a GitHub search item."""
    url = item.get("repository_url", "")
    # https://api.github.com/repos/OWNER/REPO -> OWNER/REPO
    repo = url.split("/repos/")[-1] if "/repos/" in url else ""
    return f"{repo}#{item.get('number')}"

def gh_query_items(query, limit=20):
    """Return list of search items; None on failure."""
    q = urllib.parse.quote(query, safe=":+@")
    try:
        out = subprocess.run(
            ["gh", "api", f"search/issues?q={q}&per_page={limit}"],
            capture_output=True, text=True, timeout=GH_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        log(f"gh timeout on: {query}")
        return None
    if out.returncode != 0:
        log(f"gh failed ({out.returncode}) on: {query} — {out.stderr.strip()[:120]}")
        return None
    try:
        return json.loads(out.stdout).get("items", [])
    except json.JSONDecodeError:
        return None

def poll():
    ignored = load_ignored()
    def filtered(items):
        return [i for i in items if item_key(i) not in ignored]

    raw_open    = gh_query_items("is:pr is:open author:@me")           or []
    raw_review  = gh_query_items("is:pr is:open review-requested:@me") or []
    raw_issues  = gh_query_items("is:issue is:open assignee:@me")      or []
    raw_pass    = gh_query_items("is:pr is:open author:@me status:success") or []
    raw_fail    = gh_query_items("is:pr is:open author:@me status:failure") or []
    raw_pend    = gh_query_items("is:pr is:open author:@me status:pending") or []

    open_items   = filtered(raw_open)
    review_items = filtered(raw_review)
    issue_items  = filtered(raw_issues)

    return {
        "open_prs":         len(open_items),
        "review_requested": len(review_items),
        "issues_assigned":  len(issue_items),
        "ci_passed":        len(filtered(raw_pass)),
        "ci_failed":        len(filtered(raw_fail)),
        "ci_pending":       len(filtered(raw_pend)),
        "prs":              [{"n": i["number"], "t": i["title"]} for i in open_items[:5]],
        "reviews":          [{"n": i["number"], "t": i["title"]} for i in review_items[:5]],
        "issues":           [{"n": i["number"], "t": i["title"]} for i in issue_items[:5]],
    }

def push(payload):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(ESP32_URL, data=data,
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status == 200
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        log(f"push failed: {e}")
        return False

_stop = False
def _on_signal(*_): global _stop; _stop = True; log("stopping...")

def main():
    if shutil.which("gh") is None:
        log("FATAL: gh CLI not in PATH")
        return 1
    signal.signal(signal.SIGINT,  _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)
    log(f"polling github every {POLL_S}s, pushing to {ESP32_URL}")
    while not _stop:
        p = poll()
        log(f"PRs={p['open_prs']} reviews={p['review_requested']} "
            f"issues={p['issues_assigned']}  "
            f"ci={p['ci_passed']}P/{p['ci_failed']}F/{p['ci_pending']}~")
        push(p)
        for _ in range(POLL_S):
            if _stop: break
            time.sleep(1)
    log("bye")
    return 0

if __name__ == "__main__":
    sys.exit(main())
