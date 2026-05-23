# CoinDeck data Worker

Replaces the two laptop daemons (`github_dashboard_daemon.py`, `claude_usage_daemon.py`)
with one Cloudflare Worker. The ESP32 fetches `/github` and `/usage` over HTTPS
from anywhere with WiFi — no LAN dependency.

The JSON payloads are byte-compatible with what the daemons used to POST.

## Setup

Run everything from this directory (`tools/coindeck-worker/`).

```bash
npx wrangler login
npx wrangler kv namespace create CACHE
```

Paste the printed `id` into `wrangler.toml` (replace `PASTE_KV_NAMESPACE_ID_HERE`).

### Secrets

```bash
# GitHub PAT — scopes: repo + read:user. Create at github.com/settings/tokens
npx wrangler secret put GITHUB_TOKEN

# Refresh token from ~/.claude/.credentials.json -> claudeAiOauth.refreshToken
npx wrangler secret put CLAUDE_REFRESH_TOKEN
```

Grab the refresh token with:

```bash
python3 -c "import json; print(json.load(open('$HOME/.claude/.credentials.json'))['claudeAiOauth']['refreshToken'])"
```

### Deploy

```bash
npx wrangler deploy
```

Wrangler prints a URL like `https://coindeck.<your-subdomain>.workers.dev`.
Put it in `src/secrets.h` as `WORKER_BASE_URL` and reflash the ESP32.

### Verify

```bash
curl https://coindeck.<your-subdomain>.workers.dev/github | jq
curl https://coindeck.<your-subdomain>.workers.dev/usage  | jq
```

Tail logs while testing:

```bash
npx wrangler tail
```

## Hiding noisy PRs / reviews

Edit the `IGNORED` set at the top of `src/index.js` and redeploy. Each entry
is `"owner/repo#NN"`, matching how the GitHub Search API identifies issues.

## Notes

- Anthropic OAuth rotates refresh tokens. The Worker stores the rotated value
  in KV under `claude_refresh_token`; the secret is only the seed. If you ever
  need to reset, `wrangler kv key delete --binding=CACHE claude_refresh_token`
  and `wrangler kv key delete --binding=CACHE claude_access_token`.
- `/github` is cached 120s, `/usage` 60s, matching the ESP poll cadence.
- Old daemons (`tools/*_daemon.py`, `tools/*-coindeck.service`) are obsolete.
  Stop them with `systemctl --user disable --now claude-coindeck github-coindeck`.
