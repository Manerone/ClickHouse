#include <Processors/QueryPlan/Optimizations/Optimizations.h>

#include <AggregateFunctions/IAggregateFunction.h>
#include <Interpreters/ActionsDAG.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Common/logger_useful.h>

namespace DB::QueryPlanOptimizations
{

/// PROTOTYPE: skip granules that cannot contain `max(k)` or `min(k)` for any group of `GROUP BY <keys>`,
/// where `<keys>` (in any order) form a prefix of the sorting key and `k` is the next sorting key column.
///
/// Within a part, rows are sorted by `(<keys>, k)`. The primary index stores the first row of every granule.
/// If granules `j` and `j + 1` start with the same `<keys>` prefix, then every row of granule `j` has that
/// prefix, and its `k` is not greater than `k` of the first row of granule `j + 1`. So granule `j` holds
/// neither a new group nor the maximum of an existing one, and can be skipped.
///
/// For the minimum, granule `j` must also not hold the first row of its group. That is guaranteed when
/// granule `j - 1` starts with the same prefix too, because then the group already started at or before the
/// first row of granule `j - 1`. So with `min`, granule `j` is skipped only when granules `j - 1`, `j` and
/// `j + 1` all start with the same prefix. This condition implies the one for the maximum, so it also serves
/// queries with both.
///
/// Supported aggregates: `max(k)`, `argMax(x, k)`, `min(k)` and `argMin(x, k)`. Corner cases (filters,
/// lightweight deletes, patch parts, FINAL, nullable or floating-point keys, reverse sorting keys, ...) are not
/// handled yet.

/// Follows `name` from the output of the chain of expressions down to the reading step, through
/// pass-through nodes (aliases of inputs) only.
static std::optional<String> mapToReadingColumn(String name, const std::vector<const ExpressionStep *> & expressions)
{
    for (const auto * expression : expressions)
    {
        const auto * node = expression->getExpression().tryFindInOutputs(name);
        if (!node)
            return {};
        while (node->type == ActionsDAG::ActionType::ALIAS)
            node = node->children.front();
        if (node->type != ActionsDAG::ActionType::INPUT)
            return {};
        name = node->result_name;
    }
    return name;
}

static void tryApply(AggregatingStep & aggregating, QueryPlan::Node & aggregating_node)
{
    if (aggregating.isGroupingSets())
        return;

    const auto & params = aggregating.getParams();
    if (params.only_merge || params.overflow_row || params.aggregates.empty())
        return;

    /// Collect the expressions between the aggregation and the reading step.
    std::vector<const ExpressionStep *> expressions;
    QueryPlan::Node * node = &aggregating_node;
    ReadFromMergeTree * reading = nullptr;
    while (node->children.size() == 1)
    {
        node = node->children.front();
        if (auto * expression = typeid_cast<ExpressionStep *>(node->step.get()))
        {
            expressions.push_back(expression);
            continue;
        }
        reading = typeid_cast<ReadFromMergeTree *>(node->step.get());
        break;
    }

    if (!reading || !node->children.empty())
        return;

    if (reading->getPrewhereInfo() || reading->getRowLevelFilter() || reading->isQueryWithFinal()
        || reading->isQueryWithSampling() || reading->isParallelReadingFromReplicas() || reading->isParallelReadingEnabled())
        return;

    const auto & metadata = reading->getStorageMetadata();
    const auto & sorting_key = metadata->getSortingKey();
    const size_t prefix_size = params.keys.size();

    if (sorting_key.column_names.size() <= prefix_size || metadata->getPrimaryKey().column_names.size() < prefix_size)
        return;

    NameSet prefix(sorting_key.column_names.begin(), sorting_key.column_names.begin() + prefix_size);
    for (const auto & key : params.keys)
    {
        auto column = mapToReadingColumn(key, expressions);
        if (!column || !prefix.erase(*column))
            return;
    }

    /// Whether some aggregate needs the first row of each group (`min`, `argMin`), not only the last one.
    bool need_group_start = false;
    const String & next_column = sorting_key.column_names[prefix_size];
    for (const auto & aggregate : params.aggregates)
    {
        const String name = aggregate.function->getName();
        size_t key_argument = 0;
        if ((name == "max" || name == "min") && aggregate.argument_names.size() == 1)
            key_argument = 0;
        else if ((name == "argMax" || name == "argMin") && aggregate.argument_names.size() == 2)
            key_argument = 1;
        else
            return;

        auto column = mapToReadingColumn(aggregate.argument_names[key_argument], expressions);
        if (!column || *column != next_column)
            return;

        if (name == "min" || name == "argMin")
            need_group_start = true;
    }

    auto analysis = reading->getAnalyzedResult();
    if (!analysis)
        analysis = reading->selectRangesToRead();
    if (!analysis || analysis->readFromProjection())
        return;

    auto result = std::make_shared<ReadFromMergeTree::AnalysisResult>(*analysis);

    size_t sum_marks = 0;
    size_t sum_ranges = 0;
    size_t sum_rows = 0;
    for (auto & part : result->parts_with_ranges)
    {
        const auto & data_part = part.data_part;
        auto index = data_part->getIndex();
        if (index->size() >= prefix_size && !index->empty() && !data_part->hasLightweightDelete())
        {
            const auto & columns = *index;
            const size_t index_rows = columns.front()->size();
            const size_t num_granules = data_part->index_granularity->getMarksCountWithoutFinal();

            auto same_prefix = [&](size_t lhs, size_t rhs)
            {
                for (size_t i = 0; i < prefix_size; ++i)
                    if (columns[i]->compareAt(lhs, rhs, *columns[i], 1) != 0)
                        return false;
                return true;
            };

            MarkRanges new_ranges;
            for (const auto & range : part.ranges)
            {
                for (size_t mark = range.begin; mark < range.end; ++mark)
                {
                    bool keep = mark + 1 >= num_granules || mark + 1 >= index_rows || !same_prefix(mark, mark + 1);
                    if (need_group_start)
                        keep = keep || mark == 0 || !same_prefix(mark - 1, mark);
                    if (!keep)
                        continue;
                    if (!new_ranges.empty() && new_ranges.back().end == mark)
                        ++new_ranges.back().end;
                    else
                        new_ranges.push_back(MarkRange(mark, mark + 1));
                }
            }
            part.ranges = std::move(new_ranges);
            part.exact_ranges.clear();
        }

        sum_ranges += part.ranges.size();
        sum_marks += part.getMarksCount();
        sum_rows += part.getRowsCount();
    }

    std::erase_if(result->parts_with_ranges, [](const auto & part) { return part.ranges.empty(); });

    LOG_DEBUG(getLogger("optimizeAggregationMaxBySortingKey"), "Kept {} of {} marks", sum_marks, result->selected_marks);

    result->selected_parts = result->parts_with_ranges.size();
    result->selected_ranges = sum_ranges;
    result->selected_marks = sum_marks;
    result->selected_rows = sum_rows;
    result->index_stats.emplace_back(ReadFromMergeTree::IndexStat{
        .type = ReadFromMergeTree::IndexType::MaxBySortingKey,
        .description = need_group_start
            ? "Skip granules that cannot contain the minimum or maximum of the next sorting key column"
            : "Skip granules that cannot contain the maximum of the next sorting key column",
        .num_parts_after = result->selected_parts,
        .num_granules_after = sum_marks});

    reading->setAnalyzedResult(std::move(result));
}

void optimizeAggregationMaxBySortingKey(QueryPlan::Node & root)
{
    Stack stack;
    stack.push_back({.node = &root});

    while (!stack.empty())
    {
        auto & frame = stack.back();

        if (frame.next_child < frame.node->children.size())
        {
            stack.push_back({.node = frame.node->children[frame.next_child++]});
            continue;
        }

        auto * node = frame.node;
        stack.pop_back();

        if (auto * aggregating = typeid_cast<AggregatingStep *>(node->step.get()))
            tryApply(*aggregating, *node);
    }
}

}
