# Store and durability

The document store (`modx/store.h`): collections, mutations, group commit, the change ring, compaction,
the read-only state after an I/O failure, and what happens on open after a crash or damage.

## Model

All rows live in memory, one hash map per collection; a row is a JSON object with a string `"id"`. Every
`put` / `del` gets the next `seq`, is applied in memory at once and queued as a journal record. Durability
comes from two files in the data directory:

```
snapshot.json   {"crc32":"<hex8>/<len12>","seq":N,"collections":{"<name>":[row,...],...}}
journal.log     one record per change: "<crc32 hex8> <change json>\n"
```

A record is `{"seq","entity","op","id","data","client_id"?, <journal extras>, "ts"}`, prefixed by the CRC-32
of the JSON. Open loads the snapshot and replays the journal records after its seq.

## Collections

```c
enum { C_ITEMS, C_SESSIONS, C_NCOLL };
static const char *const ITEM_INDEX[] = { "list", NULL };
static const mx_coll_def COLLS[C_NCOLL] = {
    { "items", 0, ITEM_INDEX },                              /* indexed by the string field "list" */
    { "sessions", MX_COLL_PRIVATE | MX_COLL_NO_RING, NULL },  /* never published, no ring slot */
};
mx_store *st = mx_store_open("./data", COLLS, C_NCOLL, 10000, &hooks /* or NULL */, err, sizeof err);
```

Flags: `MX_COLL_PRIVATE` (persisted, never a public change: sessions, secrets), `MX_COLL_NO_RING` (no slot in
the change ring: high-volume internal rows), `MX_COLL_REDACT` (public change JSON built with
`hooks.write_public`, e.g. without password hashes). Hooks (`mx_store_hooks`) let the application choose
storage keys (`row_key`, e.g. per-tenant), add journal-only fields (`journal_extra`), attach a visibility scope
to each ring entry (`scope_new` / `scope_free`), keep derived state (`on_apply`) and prune before a snapshot
(`before_snapshot`).

Reads: `mx_store_get(st, coll, key)` (borrowed), `mx_store_all()`, `mx_store_count()`, `mx_store_find()` /
`mx_store_find_count()` (indexed fields use the index, others scan). Writes: `mx_store_put(st, coll, obj,
client_id)` takes ownership of `obj`; `mx_store_del()` / `mx_store_del_key()`.

## Group commit

Mutations only queue records. `mx_store_sync()` writes everything queued with one `write()`, calls
`fsync()` once, and then calls `st->listener` for every newly durable public change in seq order (SSE
fan-out). With the HTTP server's barrier (`mx_store_pending_cb` / `mx_store_commit_cb`) this happens once per
loop iteration and every response that saw a pending change waits for it: an acknowledged write is on disk.
`mx_store_dirty()` says whether anything is pending.

## Change ring and catch-up

The last `ring_cap` public changes are kept with their serialized JSON and scope.
`mx_store_for_each_since(st, since, keep, keep_ctx, fn, ctx)` calls `fn` for each durable change after
`since` that `keep` accepts; it returns -1 when `since` is older than the ring or newer than the store
(the client must reload: answer 410 / send `event: reset`). `mx_store_ring_at()` walks it directly.

## Compaction

Past `st->compact_threshold` bytes of journal (4 MiB by default) `mx_store_sync()` starts a background
compaction: `journal.log` is renamed to `journal.old`, a fresh `journal.log` takes new records, and a forked
child writes `snapshot.json` from its copy-on-write image (tmp file, fsync, rename, directory fsync). The
loop is blocked only for the rename and `fork()`; `mx_store_tick()` (from the server tick) reaps the child
and removes `journal.old`. `mx_store_compact()` does the same synchronously. A crash at any point loses
nothing: open replays snapshot + `journal.old` + `journal.log`.

Threads: fork only from the loop thread. The child serializes and writes the snapshot and exits; it never
touches the worker pool, and pool workers hold no lock it could need (they do not malloc while hashing).
Never call store functions from pool jobs.

## When a write fails: read-only until reopened

If the journal `write()` or `fsync()` fails, the store becomes read-only for good:

- the failed batch is never announced durable, and `fsync()` is never retried (after a failed fsync the
  kernel may have dropped the dirty pages; a later "successful" fsync proves nothing);
- `mx_store_sync()` returns -1 for that batch and `MX_STORE_EFAILED` (-2) afterwards; mutations return -1 with
  `errno = EROFS`; compaction is refused (memory holds changes that never reached the disk);
- reads keep working; the catch-up feed stops at `synced_seq`;
- with the HTTP barrier the held responses become 500, later writes 503;
- `mx_store_failed()` and `mx_store_health_get()` (`failed`, `fail_op`, `fail_errno`, `synced_seq`,
  `rejected_writes`, ...) expose it for a health endpoint.

Recovery is a restart (reopen), which replays exactly what reached the disk.

## Recovery on open

A crash can only tear the end of the live journal: a record half written, a record without its newline,
bytes the file system appended. So on open:

- **Torn tail: cut.** Everything after the last record with a valid CRC in `journal.log` is truncated
  away and logged (`modx: journal: discarding N bytes of torn/corrupt tail at offset X`);
  `st->recovered_truncated` and `st->recovered_tail_bytes` say so.
- **Anything else: the open fails** with a message naming the file, byte offset and line, because replaying
  past it would silently drop acknowledged data:
  - a bad record (CRC mismatch, not a change object) with valid records after it;
  - a seq gap (`seq 7 after 5`) or a seq going backwards, after the snapshot's seq (records the snapshot
    already covers are skipped, as after a compaction whose journal truncate failed);
  - any damage in `journal.old` (it was complete and fsynced before it was rotated);
  - a snapshot that fails its checksum, or does not parse.
- Records of a collection this build does not know (written by a newer version) are skipped with their
  seq kept (`st->skipped_records`), so a rollback does not cut the journal.

A snapshot rewritten by another tool (an operator editing it with `jq`, a test with `JSON.stringify`) keeps
the old `crc32` key but has another length than the header records; it is loaded without the checksum and a
warning is logged (`st->snapshot_unchecked`). An in-place edit that keeps the length fails the checksum:
delete the `"crc32"` key to load such a file.

### Salvage mode

When the open fails on damage, an operator decides. To recover what is valid:

```sh
MODX_STORE_SALVAGE=1 ./server        # once; or in code:
```

```c
mx_store_opts o = { .salvage = 1, .report_path = NULL };
mx_store *st = mx_store_open_ex(dir, COLLS, C_NCOLL, 10000, &hooks, &o, err, sizeof err);
```

Salvage keeps every valid record before and after damaged ones, accepts seq gaps, skips records whose seq
goes backwards, and loads a snapshot that fails its checksum if it still parses. Then it:

1. keeps the damaged files' original bytes as `<file>.damaged-<unix ms>`;
2. writes a report, `<dir>/recovery-<unix ms>.txt` (or `report_path`): every skipped byte range with file,
   offset, lines and reason, every gap, the seq recovered;
3. compacts the recovered state into a fresh snapshot and an empty journal, so the next open is clean
   without salvage.

`st->salvaged_records`, `salvaged_bytes`, `seq_gaps`, `snapshot_crc_mismatch` and `recovery_report` describe
what happened. Salvage cannot invent lost records: changes inside a damaged range are gone, and the report
says which seqs were missing.

### Compatibility

Journals and snapshots written by 0.1 and 0.2 open unchanged (the journal format did not change; a
snapshot without the `crc32` key is read without a check). Snapshots written now are still plain JSON with
one more key, so 0.2 can read them.

## Testing hooks

`st->fault_syncs = 1` makes the next sync fail as an fsync error would; `st->sync_compaction = 1` compacts in
process; `st->test_crash_compaction = 1` kills the compaction child halfway. `tests/test_store.c` cuts the
journal at every byte offset, flips bits, forges gaps and damaged `journal.old` files, and `make fuzz`
mutates journals and snapshots (refused inputs are then salvaged and must reopen cleanly).

See also: [docs index](../README.md) · [API reference](../reference.md) · [HTTP and routing](http.md) ·
[Server-Sent Events](sse.md) · [Idempotency keys](idempotency.md)
