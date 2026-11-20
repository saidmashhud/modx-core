// End-to-end test of examples/todo: CRUD, SSE fan-out and catch-up, /changes,
// persistence across restarts, static files. Usage: node tests/todo.mjs ./build/todo
// Node >= 18, no dependencies.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import net from 'node:net';

const BIN = resolve(process.argv[2] || './build/todo');
const root = mkdtempSync(join(tmpdir(), 'modx-todo-'));
const data = join(root, 'data');
const pub = join(root, 'public');
mkdirSync(pub);
writeFileSync(join(pub, 'index.html'), '<!doctype html><title>t</title>');
writeFileSync(join(pub, 'app.js'), 'console.log(1);'.repeat(200));

let passed = 0, failed = 0;
const check = (c, msg, extra) => { if (c) passed++; else { failed++; console.error('  FAIL:', msg, extra === undefined ? '' : JSON.stringify(extra).slice(0, 300)); } };
const section = (s) => console.log('- ' + s);

let proc = null, base = '';
function start(env = {}) {
  return new Promise((res, rej) => {
    proc = spawn(BIN, [], { env: { ...process.env, TODO_PORT: '0', TODO_DATA: data, TODO_STATIC: pub, TODO_ADMIN: '1', ...env }, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    const t = setTimeout(() => rej(new Error('no start: ' + out)), 20000);
    proc.stdout.on('data', (d) => {
      out += d;
      const m = out.match(/listening on http:\/\/([\d.]+):(\d+)/);
      if (m) { clearTimeout(t); base = `http://${m[1]}:${m[2]}`; res(); }
    });
    proc.stderr.on('data', (d) => process.stderr.write('  [todo] ' + d));
    proc.on('exit', (c) => { clearTimeout(t); if (!base) rej(new Error('exited ' + c)); });
  });
}
const stop = () => new Promise((res) => { if (!proc || proc.exitCode !== null) return res(); proc.once('exit', (c) => res(c)); proc.kill('SIGTERM'); });

async function api(method, path, body, headers = {}) {
  const r = await fetch(base + path, { method, headers: { ...(body ? { 'Content-Type': 'application/json' } : {}), ...headers }, body: body ? JSON.stringify(body) : undefined });
  const text = await r.text();
  let json = null;
  try { json = JSON.parse(text); } catch { /* */ }
  return { status: r.status, json, text, headers: r.headers };
}

// Minimal SSE reader on a raw socket (records every event).
function sse(path, headers = {}) {
  const u = new URL(base + path);
  const events = [];
  let buf = '';
  const sock = net.connect(Number(u.port), u.hostname);
  sock.write(`GET ${u.pathname}${u.search} HTTP/1.1\r\nHost: ${u.host}\r\nAccept: text/event-stream\r\n${Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('')}\r\n`);
  sock.on('data', (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf('\n\n')) >= 0) {
      const block = buf.slice(0, i);
      buf = buf.slice(i + 2);
      const ev = { id: null, event: 'message', data: '' };
      for (const line of block.split('\n')) {
        if (line.startsWith('id: ')) ev.id = Number(line.slice(4));
        else if (line.startsWith('event: ')) ev.event = line.slice(7);
        else if (line.startsWith('data: ')) ev.data += line.slice(6);
      }
      if (ev.data) events.push(ev);
    }
  });
  return { events, close: () => sock.destroy() };
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(fn, ms = 3000) { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(20); } return false; }

try {
  await start();
  section('health, static');
  check((await api('GET', '/api/health')).json?.ok === true, 'health');
  const idx = await api('GET', '/');
  check(idx.status === 200 && idx.text.includes('<title>t</title>'), 'index.html');
  check(!(await api('GET', '/%2e%2e/%2e%2e/etc/passwd')).text.includes('root:'), 'no traversal');

  section('CRUD');
  const live = sse('/api/events');
  await sleep(150);
  const c1 = await api('POST', '/api/todos', { title: 'Buy milk', list: 'home' }, { 'X-Client-Id': 'tab-1' });
  check(c1.status === 201 && c1.json.title === 'Buy milk' && c1.json.done === false, 'create', c1.json);
  const id = c1.json.id;
  check((await api('POST', '/api/todos', {})).status === 400, 'title required');
  check((await api('GET', `/api/todos/${id}`)).json?.list === 'home', 'get by id');
  const u = await api('PUT', `/api/todos/${id}`, { done: true });
  check(u.status === 200 && u.json.done === true, 'update');
  check((await api('PUT', '/api/todos/nope', { done: true })).status === 404, 'update missing');
  await api('POST', '/api/todos', { title: 'Write report', list: 'work' });
  const home = await api('GET', '/api/todos?list=home');
  check(home.json.length === 1 && home.json[0].id === id, 'list filter', home.json);
  check((await api('GET', '/api/todos')).json.length === 2, 'list all');
  check((await api('DELETE', `/api/todos/${id}`)).status === 204, 'delete');
  check((await api('GET', `/api/todos/${id}`)).status === 404, 'deleted');

  section('SSE fan-out and catch-up');
  check(await until(() => live.events.length >= 4), 'four live events', live.events.length);
  check(live.events[0]?.id === 1 && JSON.parse(live.events[0].data).client_id === 'tab-1', 'event 1 carries client_id');
  check(live.events.map((e) => JSON.parse(e.data).op).join() === 'put,put,put,del', 'event ops');
  live.close();
  const late = sse('/api/events', { 'Last-Event-ID': '2' });
  check(await until(() => late.events.length >= 2), 'replay after Last-Event-ID', late.events.length);
  check(late.events[0]?.id === 3, 'replay starts at 3');
  late.close();
  const ahead = sse('/api/events', { 'Last-Event-ID': '999' });
  check(await until(() => ahead.events.some((e) => e.event === 'reset')), 'reset when ahead');
  ahead.close();
  const ch = await api('GET', '/api/changes?since=1');
  check(ch.status === 200 && ch.json.length === 3, '/changes since 1', ch.json);
  check((await api('GET', '/api/changes?since=99')).status === 410, '/changes 410');

  section('persistence');
  const before = (await api('GET', '/api/todos')).json;
  await stop();
  await start();
  const after = (await api('GET', '/api/todos')).json;
  check(JSON.stringify(after) === JSON.stringify(before), 'rows survive restart');
  const st = (await api('GET', '/api/stats')).json;
  check(st.seq === 4 && st.rows === 1, 'stats after restart', st);
  check((await api('POST', '/api/compact')).json?.ok === true, 'compact');
  await stop();
  await start();
  check((await api('GET', '/api/todos')).json.length === 1, 'rows survive compaction');
} catch (e) {
  failed++;
  console.error(e);
} finally {
  await stop();
  rmSync(root, { recursive: true, force: true });
}
console.log(`\ntodo: ${passed} checks passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
