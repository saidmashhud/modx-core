// Benchmark driver for modx-core (examples/todo + bench/loadgen).
//   node bench/run.mjs [--profile quick|full] [--label name] [--only a,b] [--todo build/todo] [--loadgen build/loadgen]
// Starts the todo server on a fresh data dir seeded with N rows and runs the
// scenarios one after another (never in parallel), with short pauses between
// them. Results: bench/results/<label>.json and a Markdown table on stdout.
// Every number is measured; nothing is estimated.
import { spawn, execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir, cpus, totalmem, platform, release } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => (a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc), []));
const profile = args.profile || 'quick';
const P = {
  quick: { rows: 20000, dur: 3, conns: 32, subs: 200, writes: 50, pause: 1000, fill: 3 },
  full: { rows: 100000, dur: 8, conns: 64, subs: 1000, writes: 200, pause: 3000, fill: 8 },
}[profile];
if (!P) throw new Error('unknown profile ' + profile);
const TODO = resolve(args.todo || join(here, '..', 'build', 'todo'));
const LOADGEN = resolve(args.loadgen || join(here, '..', 'build', 'loadgen'));
const only = args.only ? new Set(args.only.split(',')) : null;
const label = args.label || `${profile}-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function hardware() {
  let cpu = cpus()[0]?.model || 'unknown', disk = 'unknown';
  try { if (platform() === 'darwin') cpu = execFileSync('sysctl', ['-n', 'machdep.cpu.brand_string']).toString().trim(); } catch { /* */ }
  try {
    if (platform() === 'darwin') disk = execFileSync('diskutil', ['info', '/']).toString().split('\n').filter((l) => /Solid State|Protocol|File System Personality/.test(l)).map((l) => l.trim().replace(/\s+/g, ' ')).join('; ');
    else disk = execFileSync('sh', ['-c', 'df -T / | tail -1']).toString().trim().replace(/\s+/g, ' ');
  } catch { /* */ }
  return { cpu, cores: cpus().length, mem_gib: +(totalmem() / 2 ** 30).toFixed(1), os: `${platform()} ${release()}`, disk, node: process.version };
}

let proc = null, base = '', port = 0, startupLine = '';
function start(dir, env = {}) {
  return new Promise((res, rej) => {
    const t0 = performance.now();
    proc = spawn('nice', ['-n', '15', TODO], { env: { ...process.env, TODO_PORT: '0', TODO_DATA: dir, TODO_ADMIN: '1', ...env }, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    const timer = setTimeout(() => rej(new Error('server did not start: ' + out)), 120000);
    proc.stdout.on('data', (d) => {
      out += d;
      const m = out.match(/listening on http:\/\/([\d.]+):(\d+).*open_ms=(\d+), startup_ms=(\d+)/);
      if (m) { clearTimeout(timer); port = +m[2]; base = `http://${m[1]}:${m[2]}`; startupLine = out.trim(); res({ wall_ms: Math.round(performance.now() - t0), open_ms: +m[3], startup_ms: +m[4] }); }
    });
    proc.stderr.on('data', (d) => { out += d; });
    proc.on('exit', (c) => { clearTimeout(timer); if (!base) rej(new Error('exited ' + c + ': ' + out)); });
  });
}
const stop = () => new Promise((res) => { if (!proc || proc.exitCode !== null) return res(); proc.once('exit', () => res()); proc.kill('SIGTERM'); });
const rssKb = () => { try { return +execFileSync('ps', ['-o', 'rss=', '-p', String(proc.pid)]).toString().trim(); } catch { return null; } };
// `nice` execs the server, so proc.pid is the server's pid.
const stats = async () => (await fetch(base + '/api/stats')).json();

function loadgen(extra) {
  return new Promise((res, rej) => {
    const p = spawn('nice', ['-n', '15', LOADGEN, '--port', String(port), ...extra], { stdio: ['ignore', 'pipe', 'inherit'] });
    let out = '';
    p.stdout.on('data', (d) => { out += d; });
    p.on('exit', (code) => { try { res({ code, ...JSON.parse(out.trim().split('\n').pop()) }); } catch (e) { rej(new Error('loadgen output: ' + out)); } });
  });
}

const want = (name) => !only || only.has(name);
const results = { label, profile, params: P, hardware: hardware(), date: new Date().toISOString(), scenarios: {} };
const root = mkdtempSync(join(tmpdir(), 'modx-bench-'));
const dir = join(root, 'data');
try {
  // seed + snapshot (TODO_SEED compacts once after seeding)
  const seedT0 = performance.now();
  await start(dir, { TODO_SEED: String(P.rows) });
  results.scenarios.seed = { rows: P.rows, wall_ms: Math.round(performance.now() - seedT0) };
  await stop();
  // cold start from the snapshot
  const s = await start(dir);
  results.scenarios.startup = { rows: P.rows, ...s, rss_kb: rssKb() };
  for (const mode of ['get', 'list', 'write', 'mixed']) {
    if (!want(mode)) continue;
    await sleep(P.pause);
    const r = await loadgen(['--mode', mode, '--conns', String(P.conns), '--duration', String(P.dur), '--rows', String(P.rows)]);
    results.scenarios[mode] = { ...r, rss_kb: rssKb() };
    console.error(`  ${mode}: ${r.rps} req/s p50 ${r.p50_ms} p99 ${r.p99_ms} ms`);
  }
  if (want('sse')) {
    await sleep(P.pause);
    const r = await loadgen(['--mode', 'sse', '--subs', String(P.subs), '--writes', String(P.writes)]);
    results.scenarios.sse = { ...r, rss_kb: rssKb() };
    console.error(`  sse: ${r.delivered}/${r.expected} p50 ${r.deliver_p50_ms} p99 ${r.deliver_p99_ms} ms`);
  }
  if (want('compact')) {
    // compaction while reads are running: how long is the loop blocked, what do reads see
    await sleep(P.pause);
    const lg = loadgen(['--mode', 'get', '--conns', '16', '--duration', String(Math.max(3, P.dur / 2)), '--rows', String(P.rows)]);
    await sleep(1000);
    const t0 = performance.now();
    const c = await (await fetch(base + '/api/compact', { method: 'POST' })).json();
    const reqMs = Math.round(performance.now() - t0);
    const reads = await lg;
    await sleep(500);
    const st = await stats();
    results.scenarios.compact = { rows: P.rows, request_ms: reqMs, blocked_ms: c.blocked_ms, last_compact_ms: st.last_compact_ms, reads_p50_ms: reads.p50_ms, reads_p99_ms: reads.p99_ms, reads_max_ms: reads.max_ms, reads_rps: reads.rps };
    console.error(`  compact: blocked ${c.blocked_ms} ms, reads max ${reads.max_ms} ms`);
  }
  if (want('recovery')) {
    // fill the journal (no compaction), then restart and time the replay
    await stop();
    await sleep(P.pause);
    await start(dir, { TODO_COMPACT_BYTES: String(1e12) });
    const before = await stats();
    await loadgen(['--mode', 'write', '--conns', '16', '--duration', String(P.fill), '--rows', String(P.rows)]);
    const after = await stats();
    await stop();
    const r = await start(dir, { TODO_COMPACT_BYTES: String(1e12) });
    results.scenarios.recovery = { rows: P.rows, journal_records: after.seq - before.seq, journal_bytes: after.journal_bytes, ...r, rss_kb: rssKb() };
    console.error(`  recovery: ${after.seq - before.seq} records in ${r.open_ms} ms`);
  }
} finally {
  await stop();
  rmSync(root, { recursive: true, force: true });
}

mkdirSync(join(here, 'results'), { recursive: true });
writeFileSync(join(here, 'results', `${label}.json`), JSON.stringify(results, null, 2) + '\n');
const S = results.scenarios, h = results.hardware;
const lines = [`### ${label} (${profile}: ${P.rows} rows, ${P.conns} conns, ${P.dur} s per run)`, '', `${h.cpu}, ${h.cores} cores, ${h.mem_gib} GiB, ${h.os}; disk: ${h.disk}`, '', '| scenario | req/s | p50 ms | p95 ms | p99 ms | RSS MiB |', '|---|---:|---:|---:|---:|---:|'];
for (const m of ['get', 'list', 'write', 'mixed']) if (S[m]) lines.push(`| ${m} | ${S[m].rps} | ${S[m].p50_ms} | ${S[m].p95_ms} | ${S[m].p99_ms} | ${(S[m].rss_kb / 1024).toFixed(1)} |`);
if (S.sse) lines.push(`| sse ${S.sse.subs} subs × ${S.sse.writes} writes (delivery) | ${S.sse.events_per_s} ev/s | ${S.sse.deliver_p50_ms} | ${S.sse.deliver_p95_ms} | ${S.sse.deliver_p99_ms} | ${(S.sse.rss_kb / 1024).toFixed(1)} |`);
lines.push('');
if (S.startup) lines.push(`- startup from snapshot (${S.startup.rows} rows): open ${S.startup.open_ms} ms, RSS ${(S.startup.rss_kb / 1024).toFixed(1)} MiB`);
if (S.compact) lines.push(`- compaction of ${S.compact.rows} rows: event loop blocked ${S.compact.blocked_ms} ms; concurrent reads p99 ${S.compact.reads_p99_ms} ms, max ${S.compact.reads_max_ms} ms`);
if (S.recovery) lines.push(`- recovery: snapshot + ${S.recovery.journal_records} journal records (${(S.recovery.journal_bytes / 2 ** 20).toFixed(1)} MiB) replayed in ${S.recovery.open_ms} ms`);
console.log(lines.join('\n'));
