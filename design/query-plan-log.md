# Persisting executed query plans

Status: draft / RFC
Issue: https://github.com/ClickHouse/ClickHouse/issues/111748

## 1. Goal {#goal}

Give users a way to see how a query was executed *after* it has already finished, without running the query again.

Today the only way to see a plan with real runtime numbers is `EXPLAIN ANALYZE`, which executes the query one more
time. That is not useful for investigating something that already happened, and often impractical: the query may be
expensive, may have failed, or may not produce the same plan again because the data, the settings, the schema or the
server version have changed in the meantime.

The end state is that every execution worth keeping leaves behind the plan it actually ran, annotated with what
happened at each operator, in a system table. That table is the single source of the information; how it is displayed
is a separate concern, and it must support two ways of looking at it:

**In SQL, from the terminal.** A plain query against the system table has to be enough — no extra tooling, no external
service:

```sql
SELECT * FROM system.query_plan_log WHERE query_id = '...'
```

Two things have to work here. Reading a single plan: the output must be something a person can read directly, which in
a terminal means an ASCII rendering of the plan tree with its per-operator metrics — what someone pastes into a ticket
or looks at over SSH on a server they just connected to. And querying *across* plans: filtering and aggregating many
stored plans to answer questions like which operator dominates a given query shape, or whether a plan changed between
two days.

**In an external, web-based UI.** The same information, exported in a machine-readable form to a tool that can draw the
plan properly: a real tree or graph, with per-operator time and row counts, zooming, collapsing, and comparison between
two executions. ASCII cannot express a plan that is a DAG rather than a tree.

Both consumers are served from the same stored data — the system table stays the single source of truth, and no
rendering is stored that cannot be derived from it. How that data is laid out is the first design choice, and it is
open: see [Storage layout of the stored plan](#open-storage-layout).

## 2. Controls {#controls}

Capturing a plan is not free, and a plan is much larger than a `system.query_log` row, so capture has to be
controllable.

* `log_query_plans` — whether the plan is captured at all. This is the switch users reach for first, and
  it is what makes the feature safe to ship: nothing is captured unless it is asked for.
* `log_query_plan_verbosity` — how much of the plan is stored. A plan can be described by operator names and their
  runtime metrics alone, or it can include the full static detail — expressions, indexes, projections, sorting,
  headers — which is what makes plans large. At least two levels are needed; more can be added later.
* `log_query_plans_probability` — the fraction of queries whose plan is captured. On a busy server, capturing every
  query is wasteful when the point is to understand a *class* of queries; a probability lets an operator leave capture
  on permanently at a fraction of the traffic.
* `log_query_plans_min_query_duration_ms` — keep the plan only for queries that ran at least this long. Slow queries
  are the ones people investigate, and this filters out the bulk of fast queries that would otherwise dominate the
  table. It mirrors `log_queries_min_query_duration_ms`, and it is what PostgreSQL's `auto_explain` uses as its main
  control. Note that it can only decide whether the captured plan is *stored*: the duration is known only once the
  query has finished, while capture must start before the query runs, so this setting saves storage rather than
  execution overhead.

## 3. Milestones {#milestones}

### Milestone 1 — MVP {#milestone-1-mvp}

A query is executed normally, its plan is captured, and a human can read it back from a system table. Nothing more: no
external UI, no machine-readable export, no tuning knobs beyond the two that make the feature usable and safe to
enable.

Scope:

* **`log_query_plans`** — the on/off switch. Off by default, so a server that does not ask for this behaves exactly as
  before.
* **`log_query_plans_probability`** — sampling, so that the feature can be left on for a fraction of a busy server's
  traffic instead of all of it.
* **The `system.query_plan_log` table**, with a simple schema: enough to identify the execution (query, time, host) and
  to render the plan as ASCII with its per-operator metrics.

Not in this milestone: `log_query_plan_verbosity`, `log_query_plans_min_query_duration_ms`, the machine-readable export
and the external UI.

The MVP schema of `system.query_plan_log` is explicitly not the final one. It is shaped by what the ASCII rendering needs, and the storage layout
question ([§5](#open-storage-layout)) stays open — deciding it will change this table. 

### Milestone 2 — better metrics and full control {#milestone-2}

The MVP proves the path end to end; this milestone makes what it stores worth reading and finishes the settings.

Scope:

* **New metrics.** Decide what an operator should report beyond what `EXPLAIN ANALYZE` collects today. Candidates:
  memory used per operator, time spent waiting on input or blocked on output (backpressure), and operator-specific
  statistics that only a given step can produce — index pruning, join algorithm and build side, spilling, hash table
  size, filter selectivity. Each candidate has to be judged on what it costs to collect and on whether it changes a
  conclusion someone would draw from the plan.
* **A cleaner interface between collecting metrics and printing them.** Today collection and rendering are entangled in
  the `EXPLAIN ANALYZE` path. Collection should produce a structure that renderers consume, so that a new metric is
  added in one place and every consumer sees it, and so that adding the machine-readable export later does not mean
  writing the whole thing a second time. This is also what lets a step contribute its own statistics without the
  rendering code knowing about that step.
* **`log_query_plan_verbosity`** — with the metric set settled, define what each level contains.
* **`log_query_plans_min_query_duration_ms`** — the store-time duration filter.

### Milestone 3 — machine-readable export and the external UI {#milestone-3}

The second consumer from [§1](#goal): a stored plan leaves the server in a form a tool can draw, and there is a tool
that draws it.

Scope:

* **The final schema of `system.query_plan_log`.** This comes first: the MVP schema was shaped by what the ASCII
  rendering needed, and by now the metric set from Milestone 2 is known, so the storage layout question
  ([§5](#open-storage-layout)) can be settled and the table given the shape it keeps. Everything below depends on it.
* **The machine-readable export.** Whatever the storage layout turns out to be
  ([§5](#open-storage-layout)), this is the format an external tool consumes, and it is a contract: versioned,
  documented, and stable enough that a UI built against it keeps working across server upgrades.
* **A first version of the external UI.** It reads plans from `system.query_plan_log` and draws them as a tree or graph
  with per-operator time and rows. The initial version only has to make one plan legible — picking a query, showing the
  shape, and showing where the time went. Comparing two executions, and everything else the goal describes, comes
  later.

#### Plan-level, not pipeline-level {#plan-level-view}

The existing `/processors-profile` page draws the *pipeline*, and a pipeline is the plan replicated once per execution
lane, so the same branch appears once per thread. TPC-H Q3 on a 32-core machine produces 1088 processor nodes over a
few dozen distinct plan steps: 305 `ExpressionTransform`, 192 `SimpleSquashingTransform`, 88 `MergeTreeSelect`, 64
`JoiningTransform`. The graph is mostly duplication, and finding the expensive part means reading the same subtree
thirty-two times.

![One read lane repeated per thread in the `/processors-profile` graph](processors-profile-duplication.png)

The screenshot shows a fragment of that graph: the same `MergeTreeSelect` → `ExpressionTransform` →
`ExpressionTransform` → `AggregatingTransform` chain, once per thread, each with its own timing, all converging on a
single `Resize`. Every lane carries the same information; only the numbers differ.

That page cannot collapse the duplicates. `EXPLAIN PIPELINE compact = 1` merges repeated chains into one node with a
multiplier, but then the node identifiers no longer match the `processor_uniq_id` values in
`system.processors_profile_log` and the per-processor overlay breaks, so the page uses `compact = 0` deliberately.

A plan-level view does not have this problem: one node per plan step, with parallelism as an attribute of the node —
number of processors, and the distribution of active time across them — rather than as thirty-two copies of the node.
That is already how `EXPLAIN ANALYZE` reports each step group, and it is the main reason the UI renders stored plans
instead of reconstructed pipelines.

#### Rendering libraries {#ui-libraries}

The UI is built on HTML and JavaScript, so that the same code can be wrapped in an Electron application, opened as a
local file, or later embedded in the website or in the server's own web UI.

Three constraints narrow the choice. Assets have to be vendored rather than loaded from a CDN, since the server must
work in an air-gapped deployment. The license has to be compatible with Apache 2.0. And the file the server serves
should be the file a developer edits, the way every page under `programs/server` works today — a generated bundle would
put two copies of the same program in the repository, one of them unreadable, with nothing but a CI check to keep them
in agreement.

There are three decisions here, not one: what computes the layout, what draws it and handles interaction, and how our
own code is written. The last one is what actually decides whether a build step exists, and it is independent of the
other two.

**Layout: Graphviz, via the already vendored [Viz.js](https://github.com/mdaines/viz-js).**
`programs/server/js/viz-standalone.js` (MIT, bundling Graphviz and Expat, 1.4 MB) is in the tree and is what
`/processors-profile` uses today. It is used here as a *layout engine only*: besides `renderString`, it exposes
`renderJSON` and supports the `json`, `dot_json`, `xdot_json`, `plain` and `plain-ext` output formats, all of which
return node and edge coordinates rather than a finished picture. So the plan is emitted as `DOT`, Graphviz assigns
positions, and we draw from those positions. This is the opposite of what `processors_profile.html` does, and it is
what lets each node be a live HTML element instead of a shape inside a rendered SVG that per-processor statistics have
to be overlaid onto by identifier.

Graphviz costs nothing new, produces better layered layouts than the alternatives, and handles DAGs, clusters and
edge routing. It runs as WebAssembly, so layout is asynchronous.

**Rendering and interaction: our own, on top of those coordinates.** Nothing off the shelf fits well enough to be worth
the dependency, and the part that is genuinely hard — the layout — is already solved by Graphviz. What remains is a
small library of our own:

* placing each operator as an HTML element at the coordinates Graphviz returned, so a node can be a card with a table
  of metrics rather than a labelled shape;
* drawing edges as paths through the returned control points, with stroke width carrying the row count so that data
  volume is visible without reading a number;
* panning, zooming, fit-to-view and centre-on-node;
* collapsing a subtree, which means re-running layout on a reduced graph and interpolating between the two coordinate
  sets;
* the per-operator colour encoding — time, rows, memory, estimation error — which is the part that decides whether the
  view is useful, and which no library would have given us anyway.

Keeping this in our own code also keeps the authoring question closed: it is plain JavaScript, so there is no build
step, no generated artifact, and the page stays editable in place like `play.html` and `dashboard.html`. The same
module can then be reused by a standalone single-file build and by the documentation site.

The alternatives were considered and rejected. [`@dagrejs/dagre`](https://github.com/dagrejs/dagre) (MIT) is a fine
layered-layout library and vendors cleanly — its ESM build is 48 KB and fully self-contained — but Graphviz is already
present and lays out better, so it would be a second dependency doing a job we can already do.
[elkjs](https://github.com/kieler/elkjs) understands ports, which maps neatly onto operator inputs and outputs, but it
is EPL-2.0 or GPL-3.0-or-later and the dual license needs a legal check before it could be a dependency.
[Cytoscape.js](https://js.cytoscape.org/) (MIT) is a complete interactive graph renderer usable with no build step, but
it draws to a canvas, so operator cards would have to be HTML overlays positioned on top of it — which fights the one
requirement that matters most here. [React Flow](https://reactflow.dev/) (`@xyflow/react`, MIT) gives the richest
node-based UI and is the natural pairing with an external layout engine, but it is React: JSX is not practically
optional, so it implies a build step and a committed bundle. That is acceptable for a standalone application or the
docs site, and not for a page served out of `programs/server`.

Prior art worth studying: [pev2](https://github.com/dalibo/pev2), the PostgreSQL execution plan visualizer (Vue,
PostgreSQL license). It solves the same problem for Postgres and is a working demonstration of the approach chosen
here — it uses a layout algorithm (`d3-flextree`) and no graph-rendering library at all, drawing nodes as HTML inside
SVG `foreignObject` elements, edges as hand-built Bézier paths weighted by row count, and pan and zoom through
`d3.zoom`. It also ships as a single self-contained HTML file that works offline, which is a plausible distribution
model here as well.

## 4. Prior art {#prior-art}

Every major vendor gives users a way to see how a query was executed after it finished, and none of them asks the user
to run the query a second time to get it. The differences are in what is stored, who renders it, and how long it is
kept.

| Vendor | Capture | Stored as | Queryable as data | UI |
|---|---|---|---|---|
| Snowflake | Always on | One row per operator: `OPERATOR_ID`, `PARENT_OPERATORS` (array), `OPERATOR_TYPE`, and three semi-structured columns — `OPERATOR_STATISTICS`, `EXECUTION_TIME_BREAKDOWN`, `OPERATOR_ATTRIBUTES` | Yes, via `GET_QUERY_OPERATOR_STATS`, for the past 14 days | Snowsight Query Profile: a DAG with the percentage of execution time per node |
| BigQuery | Always on | A document embedded in the job resource: stages containing steps, with per-stage wait, read, compute and write times for the average and the slowest worker, record counts, and a sampled timeline | Through `jobs.get` and `INFORMATION_SCHEMA.JOBS` | Console execution graph, deep-linkable per job |
| Redshift | Always on | System views: `SYS_QUERY_HISTORY`, `SYS_QUERY_DETAIL` (per-step rows, bytes, time), `SYS_QUERY_EXPLAIN` | Yes, plain SQL | Console query profiler, a pure renderer over those views |
| Databricks | Always on | A query profile document, downloadable as JSON and importable into any other workspace | Only query-level metrics, in `system.query.history`; the operator tree is not a table | SQL UI tree with per-operator rows, time and spill |
| Oracle | Automatic for statements over 5s of CPU or I/O, and for parallel ones | `V$SQL_PLAN_MONITOR`, one row per plan line, live and after the fact; reports are persisted to `DBA_HIST_REPORTS` under AWR retention, 8 days by default | Yes | `DBMS_SQL_MONITOR.REPORT_SQL_MONITOR` renders text, HTML, XML or an *active report* — a self-contained HTML file that can be attached to a ticket |
| SQL Server | Query Store, opt-in per database | Showplan XML blob in `sys.query_store_plan`, with runtime statistics aggregated per interval in `sys.query_store_runtime_stats` | Yes, but the plan itself is an opaque blob | SSMS; aimed at plan-change regressions rather than at one execution |
| Trino, Starburst | Always on | Query and stage details in Insights; `EXPLAIN (FORMAT JSON)` for the plan | Partly | Web UI stage graph; plan and stage details are dropped after 7 days |
| PostgreSQL on RDS, Aurora, Cloud SQL | `auto_explain`, opt-in | Plans written to the server log, not to a table | No — third-party tools such as pganalyze and pgDash scrape the logs | [pev2](https://github.com/dalibo/pev2), from pasted text or JSON |

### What follows from this {#prior-art-consequences}

**The architecture in [§1](#goal) is the one that shipped elsewhere.** Redshift's console profiler renders
`SYS_QUERY_DETAIL` and `SYS_QUERY_EXPLAIN` and nothing else: the system views are the contract and the UI is one of
their clients. That is exactly the split this document proposes, which is some evidence that it holds up.

**Snowflake's layout is the hybrid considered in [§5](#open-storage-layout).** Fixed columns carry identity, topology
and timing; semi-structured columns carry the parts that differ per operator type. That gives columnar storage and
ordinary cross-plan aggregation, and still lets a join step report its build side without a schema change. Note that
`PARENT_OPERATORS` is an array, so the rows encode a DAG directly and nothing has to be reassembled from a separate
document. This is an argument for Option 2 with `JSON` columns for the heterogeneous attributes, rather than for a
single JSON blob per query.

**No vendor exposes a pipeline-level view.** All of them show the plan or stage level and describe parallelism as
attributes of a node — BigQuery reports both the average-worker and the slowest-worker timing per stage, which is one
concrete answer to the question of how to summarise the distribution of active time across processors
([§3](#plan-level-view)).

**Retention is short and bounded everywhere**: 14 days in Snowflake, 8 in Oracle, 7 in Starburst. `system.query_plan_log`
should have a default TTL rather than growing without limit.

**A self-contained single file is a recurring export format** — Oracle's active report, Databricks' downloadable and
re-importable JSON, and pev2's single HTML page. It serves the "paste it into a ticket" case far better than ASCII does,
and it is what makes a plan shareable with someone who has no access to the server. Worth treating as a deliverable of
[Milestone 3](#milestone-3) rather than as an aside.

**The controls in [§2](#controls) follow the PostgreSQL lineage, not the warehouse one.** A duration threshold, a
sampling probability and a verbosity level are `auto_explain`. The warehouses capture unconditionally because their
per-operator metrics are collected anyway as part of normal execution; here capture costs something that is not already
being paid, which is why the switch exists at all.

## 5. Open design choices {#open-design-choices}

Everything listed here is deliberately unresolved and is to be settled.

### Storage layout of the stored plan {#open-storage-layout}

Belongs to [§1 Goal](#goal).

One stored representation has to serve both consumers: SQL from a terminal (a readable ASCII rendering of one plan,
plus filtering and aggregation across many plans) and a machine-readable export for an external UI. The question is
what the system table actually stores.

**Option 1 — a JSON object per query.** The plan is stored as one JSON column, and scalar functions render it as ASCII
for terminal use. The external UI reads the JSON as-is, so the export format is the storage format and nothing has to
be assembled on the way out.

* Flexible: new per-operator fields can be added without changing the table, which matters because operator-specific
  statistics (join algorithm, index pruning, spilling) differ per step type and will keep growing.
* Self-contained: one row is one complete plan, easy to hand to any external tool.
* Weaker as data: filtering and aggregating across many plans goes through JSON extraction, and compression is worse
  than columnar storage of the same values.

**Option 2 — an explicit schema with nested fields.** The table has real columns, with the per-operator information in
nested columns, and a view on top provides the human-readable rendering. The external UI queries the table (or the
view) directly instead of fetching a blob.

* Better storage: proper types and columnar compression instead of a text blob.
* Better filtering and aggregation: "which step took longest in these 10,000 plans" is an ordinary query over columns,
  with the usual index and projection support.
* Less flexible: adding a field means changing the schema, and heterogeneous per-operator attributes fit awkwardly into
  a fixed set of columns.
* The external UI has to understand the schema and reassemble the tree, rather than consuming a self-describing
  document.

Worth considering as a variant: the two are not mutually exclusive, because the storage format and the export format
need not be the same thing. A structured table plus a function that renders a plan as JSON on demand gives the columnar
benefits and still hands the UI a document; the cost is one more conversion to maintain and version.

Whatever is chosen, both entry points must be derived from the same rows, and the format the external UI consumes has
to be versioned — it becomes a contract with third-party tools.
