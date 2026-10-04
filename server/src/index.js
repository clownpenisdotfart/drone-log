// Collector and public API for the copcity.net drone map.
//
//   POST /api/ingest   nodes upload aircraft tracks (Bearer token per node)
//   GET  /api/live     aircraft heard in the last N minutes, with tracks
//   GET  /api/stats    daily counts and activity hotspots
//
// Only aircraft positions are accepted and served. Operator (pilot) location
// never reaches the server.

const MAX_TRACKS_PER_UPLOAD = 500;
const MAX_BODY_BYTES = 256 * 1024;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    try {
      if (request.method === 'OPTIONS') return cors(env, new Response(null, { status: 204 }));
      if (url.pathname === '/api/ingest' && request.method === 'POST') return await ingest(request, env);
      if (url.pathname === '/api/live' && request.method === 'GET') return cors(env, await live(url, env));
      if (url.pathname === '/api/stats' && request.method === 'GET') return cors(env, await stats(url, env));
      return json({ error: 'not found' }, 404);
    } catch (e) {
      console.error(e);
      return json({ error: 'server error' }, 500);
    }
  },

  async scheduled(_event, env) {
    const days = Number(env.RETENTION_DAYS || 365);
    await env.DB.prepare('DELETE FROM tracks WHERE ts < ?').bind(Date.now() - days * 86400000).run();
  },
};

function json(body, status = 200, headers = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'content-type': 'application/json', ...headers },
  });
}

function cors(env, res) {
  const h = new Headers(res.headers);
  h.set('access-control-allow-origin', env.ALLOWED_ORIGIN || '*');
  h.set('access-control-allow-methods', 'GET, OPTIONS');
  h.set('vary', 'origin');
  return new Response(res.body, { status: res.status, headers: h });
}

// Constant-time comparison so token checks don't leak via timing.
function safeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

function nodeForToken(env, token) {
  let map;
  try {
    map = JSON.parse(env.NODE_TOKENS || '{}');
  } catch {
    return null;
  }
  for (const [t, name] of Object.entries(map)) {
    if (token && safeEqual(t, token)) return String(name).slice(0, 64);
  }
  return null;
}

const finite = v => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const text = (v, n) => (typeof v === 'string' ? v.slice(0, n) : null);

async function ingest(request, env) {
  const auth = request.headers.get('authorization') || '';
  const node = nodeForToken(env, auth.startsWith('Bearer ') ? auth.slice(7) : '');
  if (!node) return json({ error: 'unauthorized' }, 401);

  const raw = await request.text();
  if (raw.length > MAX_BODY_BYTES) return json({ error: 'too large' }, 413);
  let body;
  try {
    body = JSON.parse(raw);
  } catch {
    return json({ error: 'bad json' }, 400);
  }
  const uptime = finite(body.uptime_ms);
  const tracks = Array.isArray(body.tracks) ? body.tracks.slice(0, MAX_TRACKS_PER_UPLOAD) : [];
  if (uptime == null) return json({ error: 'missing uptime_ms' }, 400);

  // Nodes have no clock; place each row by its age relative to upload time.
  const now = Date.now();
  const stmt = env.DB.prepare(
    `INSERT INTO tracks (ts, node, drone, transport, rssi, ua_type, status, lat, lon, alt_geo, height, speed, dir)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
  );
  const batch = [];
  for (const t of tracks) {
    const lat = finite(t.lat), lon = finite(t.lon), up = finite(t.up_ms);
    const drone = text(t.id, 32);
    if (lat == null || lon == null || up == null || !drone) continue;
    if (Math.abs(lat) > 90 || Math.abs(lon) > 180 || (lat === 0 && lon === 0)) continue;
    const ts = now - Math.max(0, uptime - up);
    batch.push(stmt.bind(ts, node, drone, text(t.transport, 16), finite(t.rssi), text(t.ua_type, 16),
      text(t.status, 16), lat, lon, finite(t.alt_geo), finite(t.height), finite(t.speed), finite(t.dir)));
  }
  batch.push(env.DB.prepare(
    `INSERT INTO nodes (name, last_seen, uploads) VALUES (?, ?, 1)
     ON CONFLICT(name) DO UPDATE SET last_seen = excluded.last_seen, uploads = uploads + 1`,
  ).bind(node, now));
  await env.DB.batch(batch);
  return json({ ok: true, stored: batch.length - 1 });
}

async function live(url, env) {
  const minutes = Math.min(Math.max(Number(url.searchParams.get('minutes')) || 30, 1), 24 * 60);
  const since = Date.now() - minutes * 60000;
  const { results } = await env.DB.prepare(
    `SELECT ts, drone, transport, ua_type, status, lat, lon, height, alt_geo, speed
       FROM tracks WHERE ts >= ? ORDER BY ts LIMIT 20000`,
  ).bind(since).all();

  const drones = new Map();
  for (const r of results) {
    let d = drones.get(r.drone);
    if (!d) {
      d = { id: r.drone, ua_type: r.ua_type, transports: [], points: [] };
      drones.set(r.drone, d);
    }
    if (r.transport && !d.transports.includes(r.transport)) d.transports.push(r.transport);
    d.status = r.status;
    d.last_ts = r.ts;
    d.speed = r.speed;
    d.points.push([r.ts, r.lat, r.lon, r.height ?? r.alt_geo]);
  }
  const nodes = await env.DB.prepare('SELECT name, last_seen FROM nodes ORDER BY name').all();
  return json(
    { generated: Date.now(), minutes, drones: [...drones.values()], nodes: nodes.results },
    200,
    { 'cache-control': 'public, max-age=15' },
  );
}

async function stats(url, env) {
  const days = Math.min(Math.max(Number(url.searchParams.get('days')) || 30, 1), 365);
  const since = Date.now() - days * 86400000;
  const [daily, hotspots, totals] = await env.DB.batch([
    env.DB.prepare(
      `SELECT date(ts / 1000, 'unixepoch') AS day, COUNT(DISTINCT drone) AS drones, COUNT(*) AS points
         FROM tracks WHERE ts >= ? GROUP BY day ORDER BY day`,
    ).bind(since),
    // ~500 m grid cells, counted by distinct aircraft per cell.
    env.DB.prepare(
      `SELECT ROUND(lat * 200) / 200.0 AS lat, ROUND(lon * 200) / 200.0 AS lon,
              COUNT(DISTINCT drone) AS drones, COUNT(*) AS points
         FROM tracks WHERE ts >= ? GROUP BY 1, 2 ORDER BY drones DESC, points DESC LIMIT 300`,
    ).bind(since),
    env.DB.prepare(
      `SELECT COUNT(DISTINCT drone) AS drones, COUNT(*) AS points, MIN(ts) AS first, MAX(ts) AS last
         FROM tracks WHERE ts >= ?`,
    ).bind(since),
  ]);
  return json(
    { generated: Date.now(), days, totals: totals.results[0], daily: daily.results, hotspots: hotspots.results },
    200,
    { 'cache-control': 'public, max-age=300' },
  );
}
