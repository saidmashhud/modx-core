// Markdown table of several bench results side by side.
//   node bench/table.mjs bench/results/paired-0-baseline.json bench/results/paired-2-group-commit.json ...
import { readFileSync } from 'node:fs';
const runs = process.argv.slice(2).map((f) => JSON.parse(readFileSync(f, 'utf8')));
const S = (r, k) => r.scenarios[k];
const f = (x, d = 0) => (x === undefined || x === null ? '—' : Number(x).toFixed(d));
const rows = [
  ['GET by id, req/s', (r) => f(S(r, 'get')?.rps)],
  ['GET by id, p99 ms', (r) => f(S(r, 'get')?.p99_ms, 2)],
  ['list of one list (limit 50), req/s', (r) => f(S(r, 'list')?.rps)],
  ['list, p99 ms', (r) => f(S(r, 'list')?.p99_ms, 1)],
  ['PUT (durable), req/s', (r) => f(S(r, 'write')?.rps)],
  ['PUT, p99 ms', (r) => f(S(r, 'write')?.p99_ms, 2)],
  ['mixed 80/10/10, req/s', (r) => f(S(r, 'mixed')?.rps)],
  ['mixed, p99 ms', (r) => f(S(r, 'mixed')?.p99_ms, 1)],
  ['GET + 2000 idle conns, req/s', (r) => f(S(r, 'idle')?.rps)],
  ['GET + 2000 idle conns, p99 ms', (r) => f(S(r, 'idle')?.p99_ms, 2)],
  ['SSE 1000 subs: delivery p50 ms', (r) => f(S(r, 'sse')?.deliver_p50_ms, 1)],
  ['SSE 1000 subs: delivery p99 ms', (r) => f(S(r, 'sse')?.deliver_p99_ms, 1)],
  ['compaction: loop blocked, ms', (r) => f(S(r, 'compact')?.blocked_ms)],
  ['compaction: worst concurrent read, ms', (r) => f(S(r, 'compact')?.reads_max_ms, 1)],
  ['startup from snapshot, ms', (r) => f(S(r, 'startup')?.open_ms)],
  ['RSS after start, MiB', (r) => f(S(r, 'startup')?.rss_kb / 1024, 1)],
  ['recovery: journal records replayed per s', (r) => f(S(r, 'recovery') && S(r, 'recovery').journal_records / (S(r, 'recovery').open_ms / 1000))],
];
console.log(`| | ${runs.map((r) => r.label.replace(/^paired-/, '')).join(' | ')} |`);
console.log(`|---|${runs.map(() => '---:').join('|')}|`);
for (const [name, get] of rows) console.log(`| ${name} | ${runs.map(get).join(' | ')} |`);
const h = runs[0].hardware;
console.log(`\n${h.cpu}, ${h.cores} cores, ${h.mem_gib} GiB RAM, ${h.os}, ${h.disk}; load average at start: ${runs.map((r) => r.hardware.loadavg_at_start[0]).join(' / ')}.`);
