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
question ([§4](#open-storage-layout)) stays open — deciding it will change this table. 

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
  ([§4](#open-storage-layout)) can be settled and the table given the shape it keeps. Everything below depends on it.
* **The machine-readable export.** Whatever the storage layout turns out to be
  ([§4](#open-storage-layout)), this is the format an external tool consumes, and it is a contract: versioned,
  documented, and stable enough that a UI built against it keeps working across server upgrades.
* **A first version of the external UI.** It reads plans from `system.query_plan_log` and draws them as a tree or graph
  with per-operator time and rows. The initial version only has to make one plan legible — picking a query, showing the
  shape, and showing where the time went. Comparing two executions, and everything else the goal describes, comes
  later.

#### Rendering libraries {#ui-libraries}

The UI can be built on HTML and JavaScript, so that the same code can be wrapped in an Electron application, opened as a
local file, or later embedded in the website or in the server's own web UI.

Three constraints narrow the choice of libraries: assets have to be vendored rather than loaded from a CDN, since the
server must work in an air-gapped deployment; the license has to be compatible with Apache 2.0; and the pages the
server serves today are plain HTML with no build step.

There are two decisions here, not one: which library computes the layout, and which one draws it and handles
interaction.

* **Graphviz via [Viz.js](https://github.com/mdaines/viz-js)** (MIT, bundling Graphviz and Expat) is already vendored
  as `programs/server/js/viz-standalone.js` and is what `/processors-profile` uses to draw pipelines from `DOT`. It
  costs nothing new, needs no build step, and produces good layered layouts. It renders a static SVG, so panning,
  zooming, collapsing and tooltips have to be added on top — which is exactly what `processors_profile.html` does
  today. The bundle is 1.4 MB.
* **[`@dagrejs/dagre`](https://github.com/dagrejs/dagre)** (MIT) computes layered DAG layouts and nothing else; the
  drawing is left to the caller. It is the maintained fork; the original `dagre` package has not been released in
  years.
* **[elkjs](https://github.com/kieler/elkjs)** (EPL-2.0 or GPL-3.0-or-later) produces the best layered layouts of the
  three and understands ports, which maps well onto operator inputs and outputs. The dual license requires a legal
  check before it can be a dependency.
* **[Cytoscape.js](https://js.cytoscape.org/)** (MIT, including its first-party extensions) is a complete interactive
  graph renderer — pan, zoom, collapse, styling — usable from plain JavaScript with no build step, with a `dagre`
  layout extension.
* **[React Flow](https://reactflow.dev/)** (`@xyflow/react`, MIT) gives the richest node-based UI, at the price of
  React and a build step. That is fine for a standalone Electron or web application and for the docs site, but it does
  not fit the server's built-in pages as they are built today.

Prior art worth studying: [pev2](https://github.com/dalibo/pev2), the PostgreSQL execution plan visualizer (Vue,
PostgreSQL license). It solves the same problem for Postgres, and it ships as a single self-contained HTML file that
works offline — a plausible distribution model here as well.

## 4. Open design choices {#open-design-choices}

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
