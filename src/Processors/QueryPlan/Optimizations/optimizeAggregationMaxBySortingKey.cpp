#include <Processors/QueryPlan/Optimizations/Optimizations.h>

#include <AggregateFunctions/IAggregateFunction.h>
#include <Core/Range.h>
#include <DataTypes/IDataType.h>
#include <Interpreters/ActionsDAG.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/Optimizations/projectionsCommon.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/KeyCondition.h>
#include <Storages/MergeTree/MergeTreeData.h>
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
/// Filters. The filter is split into conjuncts:
/// - A conjunct that depends only on the `GROUP BY` columns removes whole groups, so it does not matter.
/// - The other conjuncts must be certainly true for the row that proves the granule unnecessary: the first row of
///   granule `j + 1` for the maximum, and the first row of granule `j - 1` for the minimum. These rows are
///   real rows of the same group, and their `k` bounds every `k` in granule `j`; if they pass the filter, the
///   group's answer is not in granule `j`. They are checked on the primary index with a `KeyCondition`, which
///   must be exact (not relaxed): a conjunct on a column outside the primary index, or on a function the
///   analysis does not know, disables the optimization.
///
/// Supported aggregates: `max(k)`, `argMax(x, k)`, `min(k)` and `argMin(x, k)`.
///
/// The rule assumes that every row of the part reaches the aggregation unless the filter removes it, and that
/// the stored order is the order the aggregation uses. So it is not applied with `FINAL`, sampling, reverse
/// sorting keys or key types whose order or equality differ from the aggregation's, and parts with deleted rows,
/// patch parts or on-the-fly mutations are read in full.

/// The order and equality of the primary index must agree with the aggregation. `Nullable`: `max` and `min`
/// ignore NULLs, which sort at an end of the run. Floating point: NaN has no consistent order, and `-0` and `+0`
/// sort as equal but form different groups. `Variant`, `Dynamic` and `JSON` can hold such values at runtime.
static bool isSupportedKeyType(const IDataType & type)
{
    return !type.isNullable() && !type.isLowCardinalityNullable() && !isFloat(type) && !isVariant(type)
        && !isDynamic(type) && !isObject(type);
}

static bool isSupportedKeyTypeRecursive(const DataTypePtr & type)
{
    bool supported = isSupportedKeyType(*type);
    type->forEachChild([&](const IDataType & child) { supported = supported && isSupportedKeyType(child); });
    return supported;
}

/// Follows `name` from the outputs of the expressions between the aggregation and the reading step down to a
/// column of the reading step, through aliases only. Without expressions, the name is already a column.
static std::optional<String> mapToReadingColumn(const std::optional<ActionsDAG> & dag, const String & name)
{
    if (!dag)
        return name;

    const auto * node = dag->tryFindInOutputs(name);
    if (!node)
        return {};
    while (node->type == ActionsDAG::ActionType::ALIAS)
        node = node->children.front();
    if (node->type != ActionsDAG::ActionType::INPUT)
        return {};
    return node->result_name;
}

/// Whether `predicate` is deterministic and depends only on the columns in `columns`.
static bool dependsOnlyOn(const ActionsDAG::Node * predicate, const NameSet & columns)
{
    std::vector<const ActionsDAG::Node *> stack{predicate};
    std::unordered_set<const ActionsDAG::Node *> visited;
    while (!stack.empty())
    {
        const auto * node = stack.back();
        stack.pop_back();
        if (!visited.insert(node).second)
            continue;
        if (!node->isDeterministic() || node->type == ActionsDAG::ActionType::ARRAY_JOIN)
            return false;
        if (node->type == ActionsDAG::ActionType::INPUT && !columns.contains(node->result_name))
            return false;
        for (const auto * child : node->children)
            stack.push_back(child);
    }
    return true;
}

static void tryApply(AggregatingStep & aggregating, QueryPlan::Node & aggregating_node)
{
    if (aggregating.isGroupingSets())
        return;

    const auto & params = aggregating.getParams();
    if (params.only_merge || params.overflow_row || params.aggregates.empty() || aggregating_node.children.size() != 1)
        return;

    /// The expressions and filters between the aggregation and the reading step: only `ExpressionStep` and
    /// `FilterStep`, plus `PREWHERE` and row policies of the reading step.
    QueryDAG query;
    if (!query.build(*aggregating_node.children.front()))
        return;

    QueryPlan::Node * node = aggregating_node.children.front();
    while (node->children.size() == 1)
        node = node->children.front();
    auto * reading = typeid_cast<ReadFromMergeTree *>(node->step.get());
    if (!reading)
        return;

    if (reading->isQueryWithFinal() || reading->isQueryWithSampling() || reading->isParallelReadingFromReplicas()
        || reading->isParallelReadingEnabled())
        return;

    const auto & metadata = reading->getStorageMetadata();
    const auto & sorting_key = metadata->getSortingKey();
    const auto & primary_key = metadata->getPrimaryKey();
    const size_t prefix_size = params.keys.size();

    if (sorting_key.column_names.size() <= prefix_size || primary_key.column_names.size() < prefix_size)
        return;

    /// The prefix columns and the next column must be sorted ascending, with types whose order the aggregation shares.
    for (size_t i = 0; i <= prefix_size; ++i)
    {
        if (i < sorting_key.reverse_flags.size() && sorting_key.reverse_flags[i])
            return;
        if (!isSupportedKeyTypeRecursive(sorting_key.data_types[i]))
            return;
    }

    const NameSet group_columns(sorting_key.column_names.begin(), sorting_key.column_names.begin() + prefix_size);
    NameSet remaining_group_columns = group_columns;
    for (const auto & key : params.keys)
    {
        auto column = mapToReadingColumn(query.dag, key);
        if (!column || !remaining_group_columns.erase(*column))
            return;
    }

    /// Whether some aggregate needs the last row of each group (`max`, `argMax`) or the first one (`min`, `argMin`).
    bool need_group_end = false;
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

        auto column = mapToReadingColumn(query.dag, aggregate.argument_names[key_argument]);
        if (!column || *column != next_column)
            return;

        if (name == "min" || name == "argMin")
            need_group_start = true;
        else
            need_group_end = true;
    }

    /// Split the filter: conjuncts on the `GROUP BY` columns only remove whole groups; the other ones must hold for
    /// the witness rows, which is checked with an exact `KeyCondition` on the primary index.
    ActionsDAG::NodeRawConstPtrs witness_conjuncts;
    if (query.filter_node)
    {
        const auto * predicate = query.filter_node;
        while (predicate->type == ActionsDAG::ActionType::ALIAS)
            predicate = predicate->children.front();
        for (const auto * conjunct : ActionsDAG::extractConjunctionAtoms(predicate))
            if (!dependsOnlyOn(conjunct, group_columns))
                witness_conjuncts.push_back(conjunct);
    }

    const auto context = reading->getContext();
    std::optional<ActionsDAG> witness_dag;
    std::optional<ActionsDAGWithInversionPushDown> witness_filter;
    std::optional<KeyCondition> witness_condition;
    if (!witness_conjuncts.empty())
    {
        witness_dag = ActionsDAG::buildFilterActionsDAG(witness_conjuncts);
        if (!witness_dag)
            return;
        witness_filter.emplace(witness_dag->getOutputs().front(), context, /*boolean_context=*/ true);
        witness_condition.emplace(*witness_filter, context, primary_key);
        witness_condition->relaxRangeAtomsOverNaNHidingTupleColumns(primary_key.data_types);
        if (witness_condition->isRelaxed())
            return;
    }

    auto analysis = reading->getAnalyzedResult();
    if (!analysis)
        analysis = reading->selectRangesToRead();
    if (!analysis || analysis->readFromProjection())
        return;

    auto result = std::make_shared<ReadFromMergeTree::AnalysisResult>(*analysis);
    const auto & mutations_snapshot = reading->getMutationsSnapshot();

    /// Rows of such parts can be hidden or changed while reading, after the primary index was written:
    /// lightweight deletes, patch parts (e.g. a `DELETE` in lightweight-update mode) and mutations applied on the fly.
    auto may_hide_rows = [&](const MergeTreeData::DataPartPtr & data_part)
    {
        return data_part->hasLightweightDelete()
            || (mutations_snapshot
                && (!mutations_snapshot->getPatchesForPart(data_part).empty()
                    || !mutations_snapshot->getOnFlyMutationCommandsForPart(data_part).empty()));
    };

    size_t sum_marks = 0;
    size_t sum_ranges = 0;
    size_t sum_rows = 0;
    for (auto & part : result->parts_with_ranges)
    {
        const auto & data_part = part.data_part;
        auto index = data_part->getIndex();
        if (index->size() >= prefix_size && !index->empty() && !may_hide_rows(data_part))
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

            /// Whether the first row of granule `row` certainly passes the filter. Primary key columns that are not
            /// loaded in the index are unknown, so a conjunct on them is not certainly true.
            auto witness_passes = [&](size_t row)
            {
                if (!witness_condition)
                    return true;
                Hyperrectangle point;
                point.reserve(primary_key.column_names.size());
                for (size_t i = 0; i < primary_key.column_names.size(); ++i)
                {
                    if (i < columns.size())
                        point.emplace_back(FieldRef((*columns[i])[row]));
                    else
                        point.emplace_back(Range::createWholeUniverse());
                }
                return !witness_condition->checkInHyperrectangle(point, primary_key.data_types).can_be_false;
            };

            MarkRanges new_ranges;
            for (const auto & range : part.ranges)
            {
                for (size_t mark = range.begin; mark < range.end; ++mark)
                {
                    bool can_skip = mark + 1 < num_granules && mark + 1 < index_rows && same_prefix(mark, mark + 1);
                    if (can_skip && need_group_end)
                        can_skip = witness_passes(mark + 1);
                    if (can_skip && need_group_start)
                        can_skip = mark > 0 && same_prefix(mark - 1, mark) && witness_passes(mark - 1);
                    if (can_skip)
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
