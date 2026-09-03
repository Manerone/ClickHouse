#pragma once

#include <memory>
#include <Processors/QueryPlan/IQueryPlanStep.h>
#include <boost/container_hash/hash.hpp>

namespace DB
{

class StepWallClock;

class StepWallClockRegistry
{
public:
    StepWallClockRegistry();

    void populateFromPlan(const QueryPlan & plan);

    StepWallClock * find(const String & step_uniq_id, size_t group) const;

    UInt64 getQueryStartNs() const { return query_start_ns; }
private:

    using StepAndGroup = std::pair<String, size_t>;
    using Hash = boost::hash<StepAndGroup>;
    using StepWallClockPtr = std::unique_ptr<StepWallClock>;
    using MapStepToWallClock = std::unordered_map<StepAndGroup, StepWallClockPtr, Hash>;

    UInt64 query_start_ns = 0;
    MapStepToWallClock clocks;
};
}
