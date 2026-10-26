# Design: RDO on BLUESHELL and the Cobalt Connector (workload plugins)

Requirements: the user, 2026-10-26: "consider using BLUESHELL cores as Cobalt does"; "look at the cobalt connector app";
"Connector app may need some kind of plugin mechanism to get it to accept workloads for multiple projects".
Status: proposed
Date: 2026-10-26
Related: [gstx-rdo.md](gstx-rdo.md) (the RDO module); Cobalt's `backend/src/services/research_jobs.py`,
`docs/design/notes/contributed_cores.md`, `docs/design/notes/cobalt_connector_v1_2.md`; the Connector app,
github.com/dewilkinson/cobalt-connector.

## Summary
Cobalt spreads heavy research over a fleet: BLUESHELL (a 32-core machine on the LAN, called directly by HTTP, slots
sized by its free memory, least-loaded dispatch, this machine taking the overflow and redoing failures, a code-hash
check so a worker on other code is never sent work, a progress row with stages, throughput, ETA and the fleet table),
then partners' machines running the **Cobalt Connector**, which pull jobs through the relay (api.bluesec.ai) and
run Cobalt's Python worker code, downloaded and verified by hash. The Connector serves one project. This note proposes
(1) a SplatPak worker for the RDO search on BLUESHELL, the same way, and (2) a **workload plugin** mechanism so the
Connector can take jobs from more than one project, SplatPak among them.

## When it pays
RDO phase 1 measures about 20 candidates of one scan in about a minute on this machine (Cthulhu: 58 s), so a fleet
gains little there: shipping the scan (784 MB) and rebuilding its pyramid would cost most of the saving. It pays for:
- phase 2 and 3 searches (per-chunk choices, record precision, pruning: hundreds of candidates or units);
- the rendered check: rendering every candidate's views (`--capture-views`) needs a GPU per candidate;
- batches: re-tuning every scan of a library overnight.

## Part 1: a SplatPak worker on BLUESHELL (LAN)
- `SplatPak.exe --rdo-worker [port]`: headless, no window; serves `POST /rdo/job` (one candidate or a batch: scan hash,
  bake parameters, candidate settings) and `GET /rdo/health` (cores, free memory, slots, build hash).
- The scan is shipped once per hash and kept in the worker's cache; the worker builds the pyramid itself (deterministic,
  so every machine measures the same thing) and answers with each candidate's size and distortion, the per-region table
  included.
- The app's RDO search becomes a dispatcher: worker slots first, this machine for the overflow and anything a worker
  refuses or fails, as Cobalt's `research_jobs` does. A worker on another build hash is never sent work.
- Progress: the RDO menu's progress block lists the fleet (machine, slots, jobs done) with throughput and an ETA.
- Code and data stay on the user's own machines: BLUESHELL is the user's.

## Part 2: workload plugins in the Cobalt Connector
Today the Connector is one project end to end: one fleet server, one code hash, one runner (the embedded Python running
Cobalt's pull loop). A plugin is a project the Connector also serves:

| Piece | Today (Cobalt only) | With plugins |
|---|---|---|
| Registration | partner key for the Cobalt fleet | per project: its server, its key or pass, enabled by the partner |
| Code | Cobalt build by hash, Python | per project, by hash; a runner kind per plugin: `python` (embedded) or `native` (a signed exe run as a child process) |
| Jobs | Cobalt's pull loop | per project pull loop or queue; the Connector's supervisor shares the machine's limits (cores, memory, hours) between projects, by weights the partner sets |
| UI | jobs in flight, totals | grouped by project; pause per project |
| Trust | 5% cross-check on BLUESHELL | per project; a plugin's builds are signed by its owner and verified before running |

SplatPak would be a `native` plugin: its worker is the same `--rdo-worker` exe, pulling from a SplatPak queue instead
of serving a port.

## Decisions for the user
1. **Proprietary code on partners' machines.** A SplatPak worker carries the proprietary codec library
   (libs/gstx): any partner who runs it holds the binary. Acceptable for partners, BLUESHELL only, or not at all?
   (Proposed: BLUESHELL first; partners only after the decision.)
2. **Whose server queues SplatPak jobs**: Cobalt's relay with a project field, or a SplatPak relay of its own.
3. **Where the plugin work happens**: both Connector apps gain the plugin host -- the Windows app (C++, the public
   repository github.com/dewilkinson/cobalt-connector, not cloned locally) and the macOS app (Swift,
   `cobalt-multi-agent/installers/connector/macos`); Cobalt's server gains per-project registration. All outside this
   repository.
4. **Order**: RDO phase 1 lands locally first (it needs no fleet); then the BLUESHELL worker; then the Connector plugins.
