# Persisting executed query plans: `system.query_plan_log`

Status: draft / RFC
Issue: https://github.com/ClickHouse/ClickHouse/issues/111748
Related: https://github.com/ClickHouse/ClickHouse/issues/111352, https://github.com/ClickHouse/ClickHouse/issues/98117

## 1. Problem and goals {#problem-and-goals}

ClickHouse does not preserve the plan that a query actually executed. After the fact we can see
query-level metrics (`system.query_log`) and processor-level metrics
(`system.processors_profile_log`), but not the optimized plan itself: which steps existed, how they
were nested, which join algorithm and index were chosen, how many rows each operator produced, and
how long each operator was active.

Everything that reconstructs a plan today re-plans the query from its text (the built-in
`/processors-profile` page does exactly that via `EXPLAIN PIPELINE graph = 1`), so the reconstruction
drifts whenever settings, schema, statistics, data distribution or the server version change.
`EXPLAIN ANALYZE` gives the right information but only for a query you run *again*, which is
useless for a post-mortem and impractical for expensive queries.

Goals:

* Capture the optimized plan of a *normally executed* query, annotated with the runtime metrics
  `EXPLAIN ANALYZE` already computes, without re-running anything.
* Store it in a system table so it can be inspected, aggregated, and diffed with SQL.
* Make capture opt-in and cheap: off or sampled by default, bounded in memory and in stored size.
* Make the stored representation queryable and versioned, not a pretty string that changes shape
  between releases.

Non-goals (at least initially):

* Distributed queries are left for a future implementation.

## 2. Capture model {#capture-model}

Three properties define the behaviour, independently of how they are implemented:

* **What is captured is the optimized plan of the query that actually ran** — the plan as it looks
  after optimization, annotated with the runtime metrics `EXPLAIN ANALYZE` already produces. The
  query is neither re-planned nor re-executed, so the stored plan cannot drift from the executed
  one.
* **A query that is not selected for capture pays nothing.** The decision is taken once, from the
  settings in §6, before the query is planned. A query that is not selected takes no snapshot,
  collects no extra timing, and writes no row.
* **Failed queries are captured too**, with `type = 'ExceptionWhileProcessing'` and whatever metrics
  were collected before the query stopped. An expensive query that failed is precisely the one worth
  inspecting afterwards. If capture itself fails for any reason, the row is dropped and the query is
  unaffected.

## 3. Phase 1 — MVP {#phase-1-mvp}

**Goal:** a working end-to-end capture that a developer can already use on a support case, with the
smallest surface we are willing to keep.

**Scope**

* New system log `system.query_plan_log` (`SystemLog.h` entry, server config section
  `<query_plan_log>` with the standard `database`/`table`/`flush_interval_milliseconds`/`ttl`
  options).
* One row per query, on the host that ran it. The plan is stored as the **text** rendering — exactly
  what `EXPLAIN ANALYZE` prints today. Text first, on purpose: it reuses the existing rendering with
  no new format to design, and it makes Phase 1 shippable and reviewable on its own.
* Settings: `log_query_plans`, `log_query_plans_probability`,
  `log_query_plans_min_query_duration_ms`, `log_query_plan_verbosity`, `log_query_plan_max_size`.
* Local `SELECT` only. Distributed queries are out of scope (see §1); until they are supported, a
  distributed query simply produces no row.
* UI: none beyond `SELECT ... FROM system.query_plan_log` returning a multi-line string that renders
  correctly in `clickhouse-client` (`Vertical`/`PrettySpace`) and in the Play UI.

**Table (Phase 1)**

```sql
CREATE TABLE system.query_plan_log
(
    `hostname` LowCardinality(String),
    `event_date` Date,
    `event_time` DateTime,
    `event_time_microseconds` DateTime64(6),

    `query_id` String,
    `initial_query_id` String,
    `normalized_query_hash` UInt64,

    `query_duration_ms` UInt64,
    `type` Enum8('QueryFinish' = 2, 'ExceptionWhileProcessing' = 4),

    `revision` UInt32,
    `plan_format_version` UInt16,
    `verbosity` LowCardinality(String),

    `plan` String,              -- text rendering, as printed by EXPLAIN ANALYZE
    `plan_truncated` UInt8
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(event_date)
ORDER BY (event_date, event_time)
```

`plan_format_version` starts at 1 and is bumped whenever the meaning of `plan` changes; it exists so
that a Phase 2/3 reader can tell what it is looking at. `revision` is the server revision, for the
same reason.

**Deliverables**

1. Plan capture during normal execution, gated by the settings.
2. The `system.query_plan_log` table, its default config section, and the docs page
   `docs/en/operations/system-tables/query_plan_log.md`.
3. Settings and their documentation.
4. Stateless tests: capture on/off, sampling = 0/1, duration threshold, truncation, failed query,
   and that `log_query_plans = 0` produces no rows.
5. Performance test (§8) and its result recorded in the pull request description.

**Exit criteria**

* With `log_query_plans = 0` (the default): no measurable regression on the performance suite.
* With `log_query_plans = 1`: overhead within the budget in §8, and no query fails because plan
  capture threw — capture errors are logged and the row is skipped, never propagated to the user
  (this is the one place where swallowing an error is correct: observability must not break the
  query it observes; the failure is still visible in the server log and as a `ProfileEvent`).

## 4. Phase 2 — structured plans and tooling {#phase-2-structured-plans-and-tooling}

**Goal:** make the stored plan machine-readable and give people something better than reading a
string.

**Scope**

* Store the plan as **JSON** (`plan_json` column, `JSON` type) in addition to, or instead of, the
  text column. The same rendering also gives `EXPLAIN ANALYZE json = 1`, which does not exist today.
* `normalized_plan_hash`: a hash over the plan *shape* (step names, tree structure, and the static
  details that influence execution such as join algorithm and chosen index) excluding all runtime
  metrics, row counts and identifiers. This is what makes "did the plan change?" a `GROUP BY`.
* Additional columns: `query_kind`, `user`, `databases`/`tables`, `peak_memory_usage`,
  `planning_duration_ms`, `execution_duration_ms`, `used_settings_changed`.
* Functions over the stored plan:
  * `formatQueryPlan(plan_json[, options])` — render the JSON back to the ASCII tree, so the pretty
    output is a *view* over the data rather than the storage format;
  * `queryPlanHotPath(plan_json)` — the chain of steps accounting for most of the time;
  * `queryPlanDiff(a, b)` — structural diff of two plans.
* Built-in UI page `/query-plan` (new HTML page next to `programs/server/processors_profile.html`),
  reading directly from `system.query_plan_log`: a query picker, the plan tree with per-step time
  and row bars, and a "show JSON" toggle. Unlike `/processors-profile` it does not re-plan anything.

**JSON shape (sketch)**

```json
{
  "version": 2,
  "query": { "duration_ms": 10, "planning_ms": 6, "execution_ms": 4,
             "read_rows": 1000000, "read_bytes": 8000000, "peak_memory_bytes": 29675 },
  "root": {
    "id": "Expression_7",
    "name": "Expression",
    "description": "Project names + Projection",
    "details": { "actions": "...", "header": [ ... ] },
    "metrics": {
      "input_rows": 10, "input_bytes": 90, "output_rows": 10, "output_bytes": 90,
      "groups": [ { "group": 0, "name": "", "active_time_ns": 21820,
                    "wall_clock_ns": 22100, "num_processors": 1,
                    "elapsed_ns": { "min": 21820, "median": 21820, "max": 21820, "sum": 21820 } } ]
    },
    "children": [ ... ]
  }
}
```

Nested-columns-per-step (one row per step) was considered as an alternative to a JSON blob. JSON
wins for Phase 2 because the per-step attribute set is heterogeneous and still moving; a companion
view or table function over the JSON can expose the row-per-step shape later without changing the
storage.

**Deliverables**: JSON rendering shared with `EXPLAIN ANALYZE json = 1`, plan hash, the three
functions, the UI page, docs for all of it, tests for each function and for hash stability across
runs of the same query shape.

## 5. Phase 3 — stabilization {#phase-3-stabilization}

**Goal:** commit to the format so people can build on it.

* Freeze the JSON schema for a `plan_format_version`, document it under `docs/en/interfaces/specs/`
  or `docs/en/operations/system-tables/query_plan_log.md`, and define the compatibility policy:
  new optional fields may be added within a version; removing or changing the meaning of a field
  bumps the version; readers must ignore unknown fields.
* Decide the final table layout (single JSON column vs JSON + materialized hot columns such as
  `hot_step_name`, `total_active_time_ms`) based on real query patterns from Phases 1–2 and on the
  storage measurements.
* Extension points: a per-step `custom` object that each step type can fill in (join algorithm
  details, index pruning counters, spill statistics, hash table size, filter selectivity), so
  adding operator-specific statistics later does not touch the logging code.
* Retention and cost controls: default TTL, `max_size_rows`/`max_size_bytes` in the config section,
  and guidance for ClickHouse Cloud.
* Distributed queries: each host stores its own plan fragment, linked to the initiator through
  `initial_query_id`, with coordinator-side assembly of the full plan as a later step.
* Remaining functions: plan-regression detection helper (`normalized_query_hash`,
  `normalized_plan_hash`, quantiles), and a `queryPlanTree` table function that expands one stored
  plan into one row per step for people who prefer SQL over JSON path expressions.

## 6. Settings {#settings}

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `log_query_plans` | `Bool` | `0` | Master switch for capturing the plan of this query |
| `log_query_plans_probability` | `Float` | `1.0` | Sampling probability, evaluated once per query; sub-queries inherit the decision, so a sampled query is captured completely or not at all |
| `log_query_plans_min_query_duration_ms` | `Milliseconds` | `0` | Only store the row if the query ran at least this long. Capture still happens (the decision has to be made before execution); the filter applies at write time. Mirrors `log_queries_min_query_duration_ms` |
| `log_query_plan_verbosity` | `Enum` | `basic` | `basic` (names, descriptions, metrics), `full` (adds actions, indexes, projections, sorting, headers) — the same options `EXPLAIN PLAN` accepts |
| `log_query_plan_max_size` | `UInt64` | `1048576` | Cap on the stored plan size in bytes; larger plans are truncated and `plan_truncated = 1` |

Server-side, the table itself is configured like every other system log:

```xml
<query_plan_log>
    <database>system</database>
    <table>query_plan_log</table>
    <flush_interval_milliseconds>7500</flush_interval_milliseconds>
    <ttl>event_date + INTERVAL 14 DAY DELETE</ttl>
</query_plan_log>
```

Notes on the controls:

* `log_query_plans` defaults to `0`. Unlike `log_processors_profiles` (default `1`), a plan row is
  large and the feature is new; turning it on globally should be an explicit decision. Once the
  overhead numbers from §8 are in, a different default — or a duration-based one — can be
  revisited.
* Because capture has to start before execution, `log_query_plans_min_query_duration_ms` does not
  save the capture cost, only the storage. If that cost turns out to matter for short queries, an
  alternative is a two-level scheme: always capture the cheap skeleton, and only render the details
  for queries that pass the duration filter.

## 7. UI {#ui}

Three levels, matching the three phases:

**Phase 1 — plain text.** `SELECT plan FROM system.query_plan_log WHERE query_id = ...` returns the
same tree `EXPLAIN ANALYZE` prints:

```
Query summary:
  Time:        10.72 ms (planning 6.45 ms · execution 4.26 ms)
  Read:        1.00 million rows, 8.00 MB
  Peak memory: 28.98 KiB

Expression ((Project names + Projection))
│  I/O: rows 10 → 10 · 90 B → 90 B
│    time 21.82 us (0.5%) · parallelism 0.98/1
└──Aggregating
   │  Keys: number MOD 10
   │  Aggregates: count()
   │  I/O: rows 1.00 million → 10 (0.00%) · 1.00 MB → 90 B
   │    Stage (partial aggregation): time 868.45 us (20.4%) · parallelism 3.80/15
   │    Stage (final aggregation):   time 445.27 us (10.4%) · parallelism 1.11/16
   └──ReadFromMergeTree
         I/O: rows 0 → 1.00 million · 0 B → 8.00 MB
           time 993.94 us (23.3%) · parallelism 7.52/15
```

Cheap, immediately useful, and it is what a support engineer pastes into a ticket.

**Phase 2 — functions plus a built-in page.** Storage becomes JSON and the text tree becomes a
function over it (`formatQueryPlan`), which unlocks diffing and hot-path analysis in SQL. A new
`/query-plan` page in the embedded web UI renders the tree with time and row bars per step:

```
┌ Query 5f3c… · 10.72 ms · 1.00M rows read · 28.98 KiB peak ─────────────────┐
│ ▸ Expression (Project names + Projection)        0.5%  ▏         10 rows   │
│   ▾ Aggregating                                 30.8%  ████▏     10 rows   │
│       partial  20.4% ███▏  par 3.80/15                                     │
│       final    10.4% █▌    par 1.11/16                                     │
│     ▸ Expression (Before GROUP BY)              15.9%  ██▏       1.00M rows│
│       ▸ ReadFromMergeTree                       23.3%  ███▎      1.00M rows│
└────────────────────────────────────────────────────────────────────────────┘
```

DAG-shaped plans (joins, unions, `CTE` reuse) do not render well as ASCII; the graphical page uses
the same `viz-standalone.js` already vendored for `/processors-profile`, so a DAG is drawn as a
graph while `formatQueryPlan` keeps the tree approximation with explicit "input from" markers.

**Phase 3 — external tools.** The JSON is the contract; any external visualizer (or Grafana panel,
or the Cloud console) reads it. Nothing in the server needs to change for that beyond keeping the
format stable and documented.

## 8. Performance and storage {#performance-and-storage}

Nothing here ships without numbers. Three things must be measured, each with capture off, capture
on at `basic`, and capture on at `full`:

**1. Execution overhead.** Sources of cost, in expected order of size:

* the extra per-step timing collected during execution — proportional to the number of executor
  tasks, so it is worst on short queries that process many small chunks;
* the plan snapshot taken before execution — proportional to plan size, once per query;
* rendering and metric aggregation at query finish — proportional to plan size and processor count,
  once per query;
* the system log insert itself — amortized by the flush interval.

How to measure: a dedicated test under `tests/performance/` with a short query
(`SELECT count() FROM numbers(...)`-style, dominated by fixed overhead), a wide plan (many joins and
subqueries, dominated by plan size), and a long-running scan (where overhead should vanish into
noise); run each with `log_query_plans` off and on, and cross-check against the CI performance
comparison for the pull request. Benchmarking the snapshot and render code on its own isolates the
per-plan cost from execution noise.

Budget to hold ourselves to:

* capture disabled: no change outside noise (this is the important one — the default path);
* capture enabled, `basic`, query ≥ 100 ms: ≤ 1% wall-clock;
* capture enabled, `basic`, trivial query: report the absolute per-query cost in microseconds
  rather than a percentage, and keep it in the tens of microseconds for a small plan.

**2. Memory.** Peak additional memory per captured query = the snapshot + the rendered plan + the
collected metrics, all accounted to the query's memory tracker, so a pathological plan hits
`max_memory_usage` rather than the server. `log_query_plan_max_size` bounds the stored string; the
snapshot itself must be bounded too (a cap on the number of steps, with a marker in the row when it
trips).

**3. Storage.** Measure on a real workload rather than guessing: capture a few thousand plans from
the ClickHouse test suites and from a TPC-DS run, then report
`sum(data_compressed_bytes)`/`count()` and the size distribution per verbosity level, plus the
compression ratio for the text column versus the JSON column. Expect the tail to matter more than
the mean — TPC-DS-scale plans with `full` verbosity are the ones that decide whether the default
verbosity is `basic`. The result feeds the default TTL and the recommended sampling probability, and
those numbers go into the documentation so operators can size retention.

## 9. Risks and open questions {#risks-and-open-questions}

* **Plan lifetime.** The plan object does not normally survive until the query finishes: building
  the pipeline consumes parts of it, and the interpreter that owns it is destroyed before execution
  ends. So the plan has to be snapshotted while it is still intact and joined with the runtime
  metrics afterwards. Keeping the whole plan alive instead would be simpler but costs memory on
  every query. This is the main implementation constraint of Phase 1.
* **Reusing the existing binary plan serialization as the storage format.** Tempting, because it
  exists and is already versioned, but it serializes the plan for *execution* on another host, not
  for *inspection*: it carries sets and expression DAGs, has no place for runtime metrics, and
  cannot be queried without deserializing it. Rejected as the primary format; possibly useful later
  as an optional column for exact replay.
* **`normalized_plan_hash` stability.** It must be stable across identical executions and across
  irrelevant changes (row counts, thread counts, temporary names), and must change when the plan
  really changes. Getting this wrong makes the headline use case ("did the plan change?") useless.
  Needs its own test set, including "same query, different data volume → same hash" and
  "same query, different join algorithm → different hash".
* **Distributed queries and parallel replicas.** Deferred (§1), but the deferral must be explicit in
  the user-facing documentation, otherwise a missing row looks like a bug. Parallel replicas are the
  harder case, because the remote side runs a plan it received rather than one it planned.
* **Duplication with `system.processors_profile_log`.** Overlapping but not redundant: processors
  are the fine-grained view, plan steps the aggregated one. If both are enabled, a plan row and the
  corresponding processor rows should be joinable by step identifier; that should be stated in the
  documentation.
* **Views and sub-queries.** A query that triggers materialized views executes several pipelines.
  Which of them get plan rows, and how they are linked, needs a decision in Phase 1 (proposal: the
  main pipeline only, with view pipelines deferred to Phase 2 and linked the way
  `system.query_views_log` links them).
* **Security.** A plan contains table names, column names, index expressions and constants — the
  same class of information as `system.query_log.query`, so the same access rules apply. Worth an
  explicit check that `SELECT` on the table is not granted more widely than `query_log`.

## 10. Work breakdown {#work-breakdown}

Phase 1:

1. Make the `EXPLAIN ANALYZE` metric collection usable outside `EXPLAIN ANALYZE` (no user-visible
   change).
2. Capture the plan and its metrics during normal execution, under the settings.
3. The `system.query_plan_log` table, its config section, the settings, and the documentation.
4. Tests, the performance test, and the measured numbers in the pull request description.

Phase 2: JSON rendering (shared with `EXPLAIN ANALYZE json = 1`) → `normalized_plan_hash` →
extra columns → `formatQueryPlan` → `queryPlanHotPath` / `queryPlanDiff` → `/query-plan` UI page.

Phase 3: format freeze and specification document → per-step extension point → retention defaults →
distributed queries → remaining functions.

Each numbered item is a separate pull request against `master`.
