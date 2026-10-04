# drone-log API

Collector and public API behind the copcity.net live map. It runs on the
Cloudflare Workers free tier with a D1 (SQLite) database.

Only aircraft positions are accepted, stored and served. The nodes never
upload operator (pilot) location; that stays on each device's SD card.

| Endpoint | Who | What |
|---|---|---|
| `POST /api/ingest` | nodes (Bearer token) | upload aircraft tracks |
| `GET /api/live?minutes=30` | public | aircraft heard recently, with tracks and receiver status |
| `GET /api/stats?days=30` | public | aircraft per day and ~500 m activity hotspots |

Rows older than `RETENTION_DAYS` (default 365) are deleted daily.

## Deploy

You need a free Cloudflare account, with copcity.net added to it if you want
`api.copcity.net`.

```
cd server
npx wrangler login
npx wrangler d1 create drone-log          # paste database_id into wrangler.toml
npx wrangler d1 execute drone-log --remote --file schema.sql
npx wrangler secret put NODE_TOKENS       # e.g. {"<long random token>": "eastside-window"}
npx wrangler deploy
```

Then, in the Cloudflare dashboard, add a custom domain `api.copcity.net` to
the worker (Workers & Pages → drone-log-api → Settings → Domains & Routes).
If you use a different URL, set it in `docs/config.js`.

Give each node its own token (for example `openssl rand -hex 24`) so one can
be revoked without touching the others. The node name appears publicly in
the receiver count, so name nodes by area, not by who hosts them.

## Local test

```
npx wrangler d1 execute drone-log --local --file schema.sql
echo 'NODE_TOKENS={"testtoken":"test-node"}' > .dev.vars
npx wrangler dev --local --var ALLOWED_ORIGIN:*
```
