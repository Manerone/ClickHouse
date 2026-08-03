# Persisting executed query plans

Status: draft / RFC
Issue: https://github.com/ClickHouse/ClickHouse/issues/111748

## 1. Goal {#goal}

Give users a way to see how a query was executed *after* it has already finished, without running
the query again.

Today the only way to see a plan with real runtime numbers is `EXPLAIN ANALYZE`, which executes the
query one more time. That is not useful for investigating something that already happened, and often
impractical: the query may be expensive, may have failed, or may not produce the same plan again
because the data, the settings, the schema or the server version have changed in the meantime.

The end state is that every execution worth keeping leaves behind the plan it actually ran,
annotated with what happened at each operator, in a system table. That table is the single source
of the information; how it is displayed is a separate concern, and it must support two ways of
looking at it:

**In SQL, from the terminal.** A plain query against the system table has to be enough — no extra
tooling, no external service:

```sql
SELECT * FROM system.query_plan_log WHERE query_id = '...'
```

Two things have to work here. Reading a single plan: the output must be something a person can read
directly, which in a terminal means an ASCII rendering of the plan tree with its per-operator
metrics — what someone pastes into a ticket or looks at over SSH on a server they just connected to.
And querying *across* plans: filtering and aggregating many stored plans to answer questions like
which operator dominates a given query shape, or whether a plan changed between two days.

**In an external, web-based UI.** The same information, exported in a machine-readable form to a tool
that can draw the plan properly: a real tree or graph, with per-operator time and row counts,
zooming, collapsing, and comparison between two executions. ASCII cannot express a plan that is a DAG
rather than a tree.

Both consumers are served from the same stored data — the system table stays the single source of
truth, and no rendering is stored that cannot be derived from it. How that data is laid out is the
first design choice, and it is open: see
[Storage layout of the stored plan](#open-storage-layout).

## 2. Controls {#controls}

Capturing a plan is not free, and a plan is much larger than a `system.query_log` row, so capture
has to be controllable. Four knobs are clear already (names are provisional):

* `log_query_plans` — whether the plan of this query is captured at all. This is the switch users
  reach for first, and it is what makes the feature safe to ship: nothing is captured unless it is
  asked for.
* `log_query_plan_verbosity` — how much of the plan is stored. A plan can be described by operator
  names and their runtime metrics alone, or it can include the full static detail — expressions,
  indexes, projections, sorting, headers — which is what makes plans large. At least two levels are
  needed; more can be added later.
* `log_query_plans_probability` — the fraction of queries whose plan is captured. On a busy server,
  capturing every query is wasteful when the point is to understand a *class* of queries; a
  probability lets an operator leave capture on permanently at a fraction of the traffic.
* `log_query_plans_min_query_duration_ms` — keep the plan only for queries that ran at least this
  long. Slow queries are the ones people investigate, and this filters out the bulk of fast queries
  that would otherwise dominate the table. It mirrors `log_queries_min_query_duration_ms`, and it is
  what PostgreSQL's `auto_explain` uses as its main control. Note that it can only decide whether the
  captured plan is *stored*: the duration is known only once the query has finished, while capture
  must start before the query runs, so this setting saves storage rather than execution overhead.

## 3. Open design choices {#open-design-choices}

Everything listed here is deliberately unresolved and is to be settled in a meeting. Each entry
names the choice, the options as they stand, and the part of this document it belongs to. Once a
choice is made, the decision moves into that section and the entry is removed from this list.

### Storage layout of the stored plan {#open-storage-layout}

Belongs to [§1 Goal](#goal).

One stored representation has to serve both consumers: SQL from a terminal (a readable ASCII
rendering of one plan, plus filtering and aggregation across many plans) and a machine-readable
export for an external UI. The question is what the system table actually stores.

**Option 1 — a JSON object per query.** The plan is stored as one JSON column, and scalar functions
render it as ASCII for terminal use. The external UI reads the JSON as-is, so the export format is
the storage format and nothing has to be assembled on the way out.

* Flexible: new per-operator fields can be added without changing the table, which matters because
  operator-specific statistics (join algorithm, index pruning, spilling) differ per step type and
  will keep growing.
* Self-contained: one row is one complete plan, easy to hand to any external tool.
* Weaker as data: filtering and aggregating across many plans goes through JSON extraction, and
  compression is worse than columnar storage of the same values.

**Option 2 — an explicit schema with nested fields.** The table has real columns, with the
per-operator information in nested columns, and a view on top provides the human-readable rendering.
The external UI queries the table (or the view) directly instead of fetching a blob.

* Better storage: proper types and columnar compression instead of a text blob.
* Better filtering and aggregation: "which step took longest in these 10,000 plans" is an ordinary
  query over columns, with the usual index and projection support.
* Less flexible: adding a field means changing the schema, and heterogeneous per-operator attributes
  fit awkwardly into a fixed set of columns.
* The external UI has to understand our schema and reassemble the tree, rather than consuming a
  self-describing document.

Worth considering as a variant: the two are not mutually exclusive, because the storage format and
the export format need not be the same thing. A structured table plus a function that renders a
plan as JSON on demand gives the columnar benefits and still hands the UI a document; the cost is
one more conversion to maintain and version.

Whatever is chosen, both entry points must be derived from the same rows, and the format the
external UI consumes has to be versioned — it becomes a contract with tools we do not control.
