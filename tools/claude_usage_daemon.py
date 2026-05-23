#!/usr/bin/env python3
"""
CoinDeck daemon — polls Anthropic API every 60s, pushes rate-limit %
to the ESP32 at coindeck.local:8080/usage.

The numbers come from the API response headers Anthropic returns on
every /v1/messages call:
  anthropic-ratelimit-unified-5h-utilization   float 0..1
  anthropic-ratelimit-unified-5h-reset         unix timestamp
  anthropic-ratelimit-unified-7d-utilization   float 0..1
  anthropic-ratelimit-unified-7d-reset         unix timestamp

We use the cheapest model + 1-token max_tokens so the call costs basically
nothing — we only care about the headers, not the response body.

Run interactively for testing:
    python3 tools/claude_usage_daemon.py

Or background it:
    nohup python3 tools/claude_usage_daemon.py > /tmp/claude-daemon.log 2>&1 &
"""
import json
import signal
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

ANTHROPIC_API = "https://api.anthropic.com/v1/messages"
ESP32_URL     = "http://coindeck.local:8080/usage"
CREDS_PATH    = Path.home() / ".claude" / ".credentials.json"
POLL_S        = 60

API_HEADERS = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta":    "oauth-2025-04-20",
    "Content-Type":      "application/json",
    "User-Agent":        "coindeck-daemon/0.1",
}
API_BODY = json.dumps({
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}).encode()


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def read_token():
    try:
        d = json.loads(CREDS_PATH.read_text())
        return d.get("claudeAiOauth", {}).get("accessToken")
    except (OSError, json.JSONDecodeError) as e:
        log(f"can't read token: {e}")
        return None


def poll_once(token):
    headers = dict(API_HEADERS, Authorization=f"Bearer {token}")
    req = urllib.request.Request(ANTHROPIC_API, data=API_BODY, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            h = dict(resp.headers)
    except urllib.error.HTTPError as e:
        if e.code == 429:
            # Rate limited — still get headers
            h = dict(e.headers)
        else:
            log(f"API HTTP {e.code}: {e.read()[:160].decode(errors='replace')}")
            return None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        log(f"API call failed: {e}")
        return None

    def hdr_float(name):
        try: return float(h.get(name, 0))
        except (TypeError, ValueError): return 0.0

    def hdr_reset_min(name):
        try:
            ts = float(h.get(name, 0))
            mins = (ts - time.time()) / 60.0
            return max(0, int(round(mins)))
        except (TypeError, ValueError):
            return 0

    return {
        "session_pct":       int(round(hdr_float("anthropic-ratelimit-unified-5h-utilization") * 100)),
        "session_reset_min": hdr_reset_min("anthropic-ratelimit-unified-5h-reset"),
        "weekly_pct":        int(round(hdr_float("anthropic-ratelimit-unified-7d-utilization") * 100)),
        "weekly_reset_min":  hdr_reset_min("anthropic-ratelimit-unified-7d-reset"),
    }


def push_to_esp32(payload):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(ESP32_URL, data=data,
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        log(f"ESP32 push failed: {e}")
        return False


_stop = False
def _on_signal(_sig, _frame):
    global _stop
    _stop = True
    log("stopping...")

def main():
    signal.signal(signal.SIGINT,  _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)
    log(f"polling Anthropic every {POLL_S}s, pushing to {ESP32_URL}")

    while not _stop:
        token = read_token()
        if not token:
            log("no token, retry in 30s")
            time.sleep(30)
            continue
        payload = poll_once(token)
        if payload:
            log(f"5h={payload['session_pct']}% (reset {payload['session_reset_min']}m) | "
                f"7d={payload['weekly_pct']}% (reset {payload['weekly_reset_min']}m)")
            push_to_esp32(payload)
        # Sleep in small chunks so SIGINT is responsive.
        for _ in range(POLL_S):
            if _stop: break
            time.sleep(1)
    log("bye")


if __name__ == "__main__":
    sys.exit(main() or 0)
