#pragma once

#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/QueryPlanFormat.h>
namespace DB
{

class QueryPipeline;

class QueryPlanProfiler
{
public:
    static bool canEnableProfiler(const ContextPtr & context, bool internal);

    void setQueryPlan(QueryPlan plan_);

    QueryPlan & getQueryPlan()
    {
        chassert(query_plan.has_value());
        return query_plan.value();
    }

    bool hasQueryPlan() const { return query_plan.has_value(); }

    /// Serializes the plan as JSON and keeps the result, so that a later getPlanJSON returns it.
    /// With a pipeline the plan carries per-step runtime statistics, and the call must happen
    /// while the pipeline is alive: StepStatsStorage reads the processors, and their reports are
    /// only reachable before the pipeline is reset. Pass nullptr to render without statistics.
    /// Everything the rendering allocates, the statistics included, happens under a memory-tracker
    /// blocker, and no exception escapes.
    void render(const QueryPipeline * pipeline);

    /// The plan as JSON, for `system.query_log.query_plan`. Prefers the version produced at
    /// pipeline-finalize time, which carries per-step statistics; queries that never reached that
    /// point (failures during or before execution) are rendered here without them.
    /// Always either valid JSON or empty, never a bare diagnostic string: the caller writes it
    /// into a JSON column, which parses what it is given.
    const String & getPlanJSON()
    {
        if (!plan_json)
            render(/*pipeline=*/ nullptr);
        return *plan_json;
    }

    void setMaxDescriptionLength(size_t max_length) { max_description_length = max_length; }

    /// Instruments the pipeline so per-step timings are collected, by attaching a
    /// StepWallClockRegistry built from the captured plan. Without it the per-processor stopwatch
    /// is never started (see ExecutionThreadContext.cpp) and every step renders as
    /// "time 0.00 ns · parallelism Unknown". Must be called before execution starts.
    void instrumentPipeline(QueryPipeline & pipeline) const;

private:

    bool canRender() const { return query_plan && query_plan->isInitialized() && pretty_names.has_value(); }

    size_t max_description_length {0};
    std::optional<QueryPlan> query_plan;
    std::optional<PrettyNamesPerPlan> pretty_names;
    std::optional<String> plan_json;
};
}
