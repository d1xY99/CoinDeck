// CoinDeck data Worker — serves /github and /usage to the ESP32 over HTTPS.
//
// Same JSON shape the ESP previously received via local POSTs; the ESP now
// fetches on a timer from anywhere on the internet instead of needing the
// laptop on the same LAN.
//
// Secrets (set with `wrangler secret put NAME`):
//   GITHUB_TOKEN          GitHub PAT — scopes: repo, read:user
//   CLAUDE_REFRESH_TOKEN  refreshToken from ~/.claude/.credentials.json
//
// KV binding: CACHE  (`wrangler kv:namespace create CACHE`)
//   stores rotated refresh token, short-lived access token, and payload caches.

const CLAUDE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
const CLAUDE_TOKEN_URL = "https://console.anthropic.com/v1/oauth/token";
const CLAUDE_API_URL   = "https://api.anthropic.com/v1/messages";
const GITHUB_API       = "https://api.github.com";

// "owner/repo#NN" entries to filter out of all GitHub lists.
const IGNORED = new Set([
  // "octocat/hello#42",
]);

const CACHE_TTL_GITHUB = 120;
const CACHE_TTL_USAGE  = 60;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    try {
      if (url.pathname === "/github") return json(await getCached(env, "github_payload", CACHE_TTL_GITHUB, () => fetchGithub(env)));
      if (url.pathname === "/usage")  return json(await getCached(env, "usage_payload",  CACHE_TTL_USAGE,  () => fetchUsage(env)));
      return new Response("CoinDeck Worker — /github or /usage\n", { status: 200 });
    } catch (e) {
      return new Response(`error: ${e.message}\n${e.stack || ""}`, { status: 500 });
    }
  },
};

function json(obj) {
  return new Response(JSON.stringify(obj), {
    headers: { "Content-Type": "application/json" },
  });
}

async function getCached(env, key, ttl, build) {
  const hit = await env.CACHE.get(key, { type: "json" });
  if (hit && Date.now() - hit.ts < ttl * 1000) return hit.data;
  const data = await build();
  await env.CACHE.put(key, JSON.stringify({ data, ts: Date.now() }),
                      { expirationTtl: ttl * 4 });
  return data;
}

// ---------- GitHub ----------

async function ghSearch(env, q) {
  const u = `${GITHUB_API}/search/issues?q=${encodeURIComponent(q)}&per_page=20`;
  const r = await fetch(u, {
    headers: {
      "Authorization": `Bearer ${env.GITHUB_TOKEN}`,
      "Accept":        "application/vnd.github+json",
      "User-Agent":    "coindeck-worker",
    },
  });
  if (!r.ok) throw new Error(`gh ${r.status}: ${(await r.text()).slice(0, 200)}`);
  return (await r.json()).items || [];
}

function ghKey(item) {
  const repo = (item.repository_url || "").split("/repos/").pop() || "";
  return `${repo}#${item.number}`;
}

async function fetchGithub(env) {
  const filt = (items) => items.filter((i) => !IGNORED.has(ghKey(i)));

  const [raw_open, raw_review, raw_issues, raw_pass, raw_fail, raw_pend] = await Promise.all([
    ghSearch(env, "is:pr is:open author:@me"),
    ghSearch(env, "is:pr is:open review-requested:@me"),
    ghSearch(env, "is:issue is:open assignee:@me"),
    ghSearch(env, "is:pr is:open author:@me status:success"),
    ghSearch(env, "is:pr is:open author:@me status:failure"),
    ghSearch(env, "is:pr is:open author:@me status:pending"),
  ]);

  const open   = filt(raw_open);
  const review = filt(raw_review);
  const issues = filt(raw_issues);
  const toList = (xs) => xs.slice(0, 5).map((i) => ({ n: i.number, t: i.title }));

  return {
    open_prs:         open.length,
    review_requested: review.length,
    issues_assigned:  issues.length,
    ci_passed:        filt(raw_pass).length,
    ci_failed:        filt(raw_fail).length,
    ci_pending:       filt(raw_pend).length,
    prs:     toList(open),
    reviews: toList(review),
    issues:  toList(issues),
  };
}

// ---------- Claude usage ----------

async function getClaudeAccessToken(env) {
  const cached = await env.CACHE.get("claude_access_token");
  if (cached) return cached;

  // Rotated refresh token (if any) takes precedence over the original secret.
  const refresh = (await env.CACHE.get("claude_refresh_token")) || env.CLAUDE_REFRESH_TOKEN;

  const r = await fetch(CLAUDE_TOKEN_URL, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      grant_type:    "refresh_token",
      refresh_token: refresh,
      client_id:     CLAUDE_CLIENT_ID,
    }),
  });
  if (!r.ok) throw new Error(`claude refresh ${r.status}: ${(await r.text()).slice(0, 200)}`);
  const d = await r.json();
  const access = d.access_token;
  if (!access) throw new Error("claude refresh: no access_token in response");

  if (d.refresh_token && d.refresh_token !== refresh) {
    await env.CACHE.put("claude_refresh_token", d.refresh_token);
  }
  const ttl = Math.max(60, (d.expires_in || 3600) - 60);
  await env.CACHE.put("claude_access_token", access, { expirationTtl: ttl });
  return access;
}

async function fetchUsage(env) {
  const token = await getClaudeAccessToken(env);
  const r = await fetch(CLAUDE_API_URL, {
    method: "POST",
    headers: {
      "Authorization":     `Bearer ${token}`,
      "Content-Type":      "application/json",
      "anthropic-version": "2023-06-01",
      "anthropic-beta":    "oauth-2025-04-20",
      "User-Agent":        "coindeck-worker",
    },
    body: JSON.stringify({
      model: "claude-haiku-4-5-20251001",
      max_tokens: 1,
      messages: [{ role: "user", content: "hi" }],
    }),
  });
  // Headers carry the rate-limit data even on 429, so we don't gate on r.ok.
  const h = r.headers;
  const f = (n) => parseFloat(h.get(n) || "0") || 0;
  const resetMin = (n) => {
    const t = parseFloat(h.get(n) || "0");
    if (!t) return 0;
    return Math.max(0, Math.round((t - Date.now() / 1000) / 60));
  };
  return {
    session_pct:       Math.round(f("anthropic-ratelimit-unified-5h-utilization") * 100),
    session_reset_min: resetMin("anthropic-ratelimit-unified-5h-reset"),
    weekly_pct:        Math.round(f("anthropic-ratelimit-unified-7d-utilization") * 100),
    weekly_reset_min:  resetMin("anthropic-ratelimit-unified-7d-reset"),
  };
}
