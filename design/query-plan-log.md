# Persisting executed query plans

Status: draft / RFC
Issue: https://github.com/ClickHouse/ClickHouse/issues/111748

## 1. Goal

Give users a way to see how a query was executed *after* it has already finished, without running the query again.

Today the only way to see a plan with real runtime numbers is `EXPLAIN ANALYZE`, which executes the query one more time. That is not useful for investigating something that already happened, and often impractical: the query may be expensive, may have failed, or may not produce the same plan again because the data, the settings, the schema or the server version have changed in the meantime.

The end goal is that every execution worth keeping leaves behind the plan it actually ran, annotated with what happened at each operator, in a system table.

## 2. Non-goals

This will **not**:

* add live monitoring for query execution;
* replace any previous system tables;
* alert about plan regression;
* provide any external UI to visualize the plans;

## 3. Controls

Capturing a plan is not free, it incurs extra overhead both on processing and storage, so it has to be controllable.

* `log_query_plans` — whether the plan is captured at all. DEFAULT: 0
* `log_query_plans_profile_level` — the profiling level of the plan. 0 being the cheapest profiling information. DEFAULT: 0
* `log_query_plans_probability` — the fraction of queries whose plan is captured. DEFAULT: 1
* `log_query_plans_min_query_duration_ms` — only captures the plan if it executes for longer than `duration` in ms. DEFAULT: 0

## 4. Prior art

Every major vendor gives users a way to see how a query was executed after it finished, and none of them asks the user to run the query a second time to get it. The differences are in what is stored, who renders it, and how long it is kept.

| Vendor | Capture | Stored as | Queryable as data | UI |
|---|---|---|---|---|
| Snowflake | Always on | One row per operator: `OPERATOR_ID`, `PARENT_OPERATORS` (array), `OPERATOR_TYPE`, and three semi-structured columns — `OPERATOR_STATISTICS`, `EXECUTION_TIME_BREAKDOWN`, `OPERATOR_ATTRIBUTES` | Yes, via [`GET_QUERY_OPERATOR_STATS`](https://docs.snowflake.com/en/sql-reference/functions/get_query_operator_stats), for the past 14 days | [Snowsight Query Profile](https://docs.snowflake.com/en/user-guide/ui-snowsight-activity): a DAG with the percentage of execution time per node |
| BigQuery | Always on | A document embedded in the job resource: stages containing steps, with per-stage wait, read, compute and write times for the average and the slowest worker, record counts, and a sampled timeline | Through `jobs.get` and [`INFORMATION_SCHEMA.JOBS`](https://docs.cloud.google.com/bigquery/docs/information-schema-jobs) | [Console execution graph](https://docs.cloud.google.com/bigquery/docs/query-insights), deep-linkable per job |
| Redshift | Always on | System views: [`SYS_QUERY_HISTORY`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_HISTORY.html), [`SYS_QUERY_DETAIL`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_DETAIL.html) (per-step rows, bytes, time), [`SYS_QUERY_EXPLAIN`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_EXPLAIN.html) | Yes, plain SQL | Console query profiler, a pure renderer over those views |
| Databricks | Always on | A [query profile](https://docs.databricks.com/aws/en/sql/user/queries/query-profile) document, downloadable as JSON and importable into any other workspace | Only query-level metrics, in `system.query.history`; the operator tree is not a table | SQL UI tree with per-operator rows, time and spill |
| Oracle | Automatic for statements over 5s of CPU or I/O, and for parallel ones | [`V$SQL_PLAN_MONITOR`](https://docs.oracle.com/en/database/oracle/oracle-database/23/refrn/V-SQL_PLAN_MONITOR.html), one row per plan line, live and after the fact; reports are persisted to `DBA_HIST_REPORTS` under AWR retention, 8 days by default | Yes | [`DBMS_SQL_MONITOR.REPORT_SQL_MONITOR`](https://docs.oracle.com/en/database/oracle/oracle-database/23/arpls/DBMS_SQL_MONITOR.html) renders text, HTML, XML or an *active report* — a self-contained HTML file that can be attached to a ticket |
| SQL Server | [Query Store](https://learn.microsoft.com/en-us/sql/relational-databases/performance/monitoring-performance-by-using-the-query-store), opt-in per database | Showplan XML blob in [`sys.query_store_plan`](https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-query-store-plan-transact-sql), with runtime statistics aggregated per interval in `sys.query_store_runtime_stats` | Yes, but the plan itself is an opaque blob | SSMS; aimed at plan-change regressions rather than at one execution |
| Trino, Starburst | Always on | Query and stage details in [Insights](https://docs.starburst.io/latest/insights/query-details.html); [`EXPLAIN (FORMAT JSON)`](https://trino.io/docs/current/sql/explain.html) for the plan | Partly | Web UI stage graph; plan and stage details are dropped after 7 days |
| PostgreSQL on RDS, Aurora, Cloud SQL | [`auto_explain`](https://www.postgresql.org/docs/current/auto-explain.html), opt-in | Plans written to the server log, not to a table | No — third-party tools such as pganalyze and pgDash scrape the logs | [pev2](https://github.com/dalibo/pev2), from pasted text or JSON |

## 5. Decisions

### One row per query execution

Snowflake and Redshift store one row per operator; BigQuery and Databricks store one document per job. This proposal follows the second group: a single row holds the whole plan.

Storing it in one row means it is written atomically, versioned as a unit by `plan.format_version`, and renderable by a function that takes a single argument. It also means that reading one plan touches exactly one row, which is the dominant access pattern — a user arrives with a `query_id`.

Operator-level analytics across queries — "which step type spills most often across the fleet" — becomes an array operation, but are much less common.

### The tree lives in one column, metadata stays flat

Everything that identifies or filters an execution — `query_id`, `event_time`, `query_duration_ms`, `status` — is a top-level column. Everything that describes the plan itself lives inside the `plan` tuple.

### Statistics are aggregated per step, not per processor

`system.processors_profile_log` is the per-processor ground truth and has no notion of the plan tree. `system.query_plan_log` is the per-step aggregate that carries the structure.

### Renderings are derived, not stored

The stored form is structured; ASCII, JSON and DOT are produced from it on read by scalar functions.

`ascii_plan` in Milestone 1 is an exception, so that the table is readable by a human before any renderer exists. It is removed in Milestone 3 once the renderers land.

### Capture is off by default

Every vendor in the table above captures unconditionally. Here capture is off by default; the settings above let a user opt in at whatever sampling rate or duration threshold suits their workload.

## 6. Milestones

### Milestone 1 — MVP

A query is executed normally, its plan is captured, and a human can read it back from a system table.

#### Scope

* A new system table is introduced `system.query_plan_log`.
* The following control flags will be implemented: `log_query_plans` and `log_query_plans_probability`.

The following schema will be used for `system.query_plan_log`, where each row will contain all information related to a single query execution:

```sql

CREATE TABLE query_plan_log (
  -- Metadata
  hostname LowCardinality(String),
  event_date Date,
  event_time DateTime,
  event_time_microseconds DateTime64(6),

  query_id String,
  query_string String,
  query_duration_ms UInt64,
  revision UInt32, -- ClickHouse version

  ascii_plan String, -- Plan in plain ASCII, to be removed at M3

  status Enum ('QueryFinish', 'ExceptionBeforeStart', 'ExceptionWhileProcessing'),

  -- Plan data
  plan Tuple(
    format_version UInt16,

    -- Profiling info for what happens before execution
    pre_execution_profile Tuple (
      parser_us UInt64,
      analyzer_us UInt64,
      planner_us UInt64,
      optimizer_us UInt64
    ),

    -- Profiling of each step in the query plan
    steps Nested (
      id UInt64, -- same as `system.processors_profile_log.plan_step` for this query
      name LowCardinality(String),
      extra_info Map(LowCardinality(String), String), -- e.g. expressions
      children Array(UInt64),

      statistics Nested(
        group LowCardinality(String), -- indicates which part of the step is being executed (e.g. hash table build)
        input_rows UInt64,
        output_rows UInt64,
        estimated_output_rows UInt64,
        input_bytes UInt64,
        output_bytes UInt64,
        num_processors UInt64,
        wall_clock_us UInt64, -- wall clock time to execute the step
        sum_elapsed_time_us UInt64, -- sum of all processors elapsed time
        min_elapsed_time_us UInt64, -- min elapsed time of all processors
        max_elapsed_time_us UInt64, -- max elapsed time of all processors
        extra Map(LowCardinality(String), String) -- step specific statistics
      )
    )
  ),

  -- Added automatically by `SystemLog` for every log table that declares these columns
  INDEX event_time_index event_time TYPE minmax GRANULARITY 1,
  INDEX event_time_microseconds_index event_time_microseconds TYPE minmax GRANULARITY 1,
  INDEX query_id_index query_id TYPE bloom_filter(0.001) GRANULARITY 1
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(event_date)
ORDER BY (event_date, event_time)
```

#### Outcomes

For a query that has already finished, a user can answer, without running it again:

* which plan was executed, and which step of it dominated the runtime;
* how much each step filtered or expanded its input, in rows and in bytes, and how far that was from what the optimizer estimated;
* how many processors executed the step, if there is any skew on execution time;
* what the plan looked like for a query that failed, up to the point where it failed, if the query failed before any plan was created then nothing will be added to the `query_plan_log` table.

#### Constraints

* With `log_query_plans = 0`, or when a query is not selected by `log_query_plans_probability`, capture adds no measurable overhead: the decision is taken before any per-step accounting starts;
* At this point, when a query is selected to be profiled, there should be almost no overhead added to query performance. When `log_query_plans_profile_level` is introduced, higher profile levels will, most likely, add overhead.
* Capture never changes the plan that is executed, the result of the query, or whether it succeeds;

### Milestone 2 — Improved Control, New Metrics and Plan Comparison

#### Scope

* The following control flags will be implemented `log_query_plans_profile_level` and `log_query_plans_min_query_duration_ms`.
* Add new metrics:
  * amount of bytes spilled to disk
  * hash table sizes
  * index pruning
  * filter selectivity
  * time waiting (`input_wait_elapsed_us` / `output_wait_elapsed_us`)
* Introduce normalized query plan hash (`normalized_plan_hash`) column into `system.query_plan_log`. This will hash the "shape" of the query plan.
* Introduce normalized query hash (`normalized_query_hash`) column into `system.query_plan_log`. This will hash the query string of the query plan. The idea is that `SELECT * FROM t1 WHERE id = 1` and `SELECT * FROM t1 WHERE id = 2` hash to the same value.

#### Outcomes

A user can:

* control the level of profiling.
* measure only slow queries.
* find out which steps spilled to disk and how many bytes.
* find out if created indexes are correctly being used to filter data.
* compare identical queries across executions, over time, across hosts, or across ClickHouse versions, using `normalized_plan_hash`.
* compare identical query classes across executions, over time, across hosts, or across ClickHouse versions, using `normalized_query_hash`.

#### Constraints

* Metrics which add overhead are only calculated if `log_query_plans_profile_level` is set to allow it.
* `log_query_plans_min_query_duration_ms` records only the metrics which have no overhead. Since duration is only known once the query has finished, the threshold can filter what is stored, never what is collected — anything gated behind it must have been cheap enough to collect for every query.

### Milestone 3 — Export methods

#### Scope

* Remove the `ascii_plan` column.
* Define export methods as scalar functions, for example, `formatAsPlainAscii`, `formatAsJson`, `formatAsDot`, etc.
* Define access control.

#### Outcomes

A user can:

* visualize the query plan in different formats, even export it to an external tool;
* access the `query_plan_log` if it has access rights;

#### Constraints

* TBD

## 7. Open questions

- What granularity we should use? Maybe instead of aggregating on the steps, we should keep information about the processors and let the formatters figure out how to print.
  - Problem here is that this conflicts with `system.processors_profile_log`
- How to handle distributed queries?
- Maybe define TTL for the new system table, or let the user define somehow?
- SELECT only queries for now?