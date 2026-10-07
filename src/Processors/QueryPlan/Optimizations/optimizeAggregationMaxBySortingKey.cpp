#include <Processors/QueryPlan/Optimizations/Optimizations.h>

#include <AggregateFunctions/IAggregateFunction.h>
#include <Core/Range.h>
#include <DataTypes/IDataType.h>
#include <Interpreters/ActionsDAG.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/DistinctStep.h>
#include <Processors/QueryPlan/Optimizations/projectionsCommon.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/KeyCondition.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Common/logger_useful.h>

namespace DB::QueryPlanOptimizations
{

/// PROTOTYPE: skip granules that cannot contain `max(k)` or `min(k)` for any group of `GROUP BY <keys>`, where `k`
/// is a sorting key column and every key depends only on the sorting key columns before `k` (the prefix).
///
/// Within a part, rows are sorted by `(<prefix>, k)`, so each value of the prefix forms a run of rows sorted by `k`.
/// The primary index stores the first row of every granule. If granules `j` and `j + 1` start with the same prefix,
/// then every row of granule `j` belongs to that run, and its `k` is not greater than `k` of the first row of
/// granule `j + 1`. So granule `j` holds neither the start of a new run nor the maximum of an existing one, and can
/// be skipped. A group is a union of whole runs, because its keys are functions of the prefix, so the aggregation
/// over the remaining rows still finds the maximum of every group: it is the largest maximum of its runs.
/// For example, with `ORDER BY (exchange, symbol, ts)`, `GROUP BY exchange` with `max(ts)` keeps the last granule
/// of every `(exchange, symbol)` run, and `WHERE exchange = 'X' GROUP BY symbol` keeps the last granule of every
/// symbol of that exchange.
///
/// For the minimum, granule `j` must also not hold the first row of its run. That is guaranteed when
/// granule `j - 1` starts with the same prefix too, because then the run already started at or before the
/// first row of granule `j - 1`. So with `min`, granule `j` is skipped only when granules `j - 1`, `j` and
/// `j + 1` all start with the same prefix. This condition implies the one for the maximum, so it also serves
/// queries with both.
///
/// Filters. The filter is split into conjuncts:
/// - A conjunct that depends only on the prefix columns removes whole runs, so it does not matter.
/// - The other conjuncts must be certainly true for the row that proves the granule unnecessary: the first row of
///   granule `j + 1` for the maximum, and the first row of granule `j - 1` for the minimum. These rows are
///   real rows of the same run, and their `k` bounds every `k` in granule `j`; if they pass the filter, the
///   run's answer is not in granule `j`. They are checked on the primary index with a `KeyCondition`, which
///   must be exact (not relaxed): a conjunct on a column outside the primary index, or on a function the
///   analysis does not know, disables the optimization.
///
/// Supported aggregates: `max(k)`, `argMax(x, k)`, `min(k)` and `argMin(x, k)`, all on the same `k`, plus aggregates
/// whose result depends only on the set of distinct values of arguments that depend only on the prefix (`min`, `max`,
/// `any`, `uniq*`, `groupUniqArray`, ...), which only need every run to be present.
///
/// Without `max(k)`-like aggregates (`GROUP BY` with only such aggregates or none, and `DISTINCT`), the prefix is the
/// shortest sorting key prefix that covers the keys and arguments, and the last granule of each run is kept, as for
/// `max`: then every run is present.
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

/// Aggregate functions whose result depends only on the set of distinct values of their arguments, not on how
/// many rows have each value. Over arguments that depend only on the prefix columns, they only need every run to
/// be present, which keeping at least one granule per run guarantees.
static bool dependsOnlyOnDistinctValues(const String & function_name)
{
    static const std::unordered_set<String> names{
        "min", "max", "any", "uniq", "uniqExact", "uniqCombined", "uniqCombined64", "uniqHLL12", "uniqTheta",
        "groupUniqArray", "groupBitOr", "groupBitAnd", "sumDistinct", "avgDistinct"};
    return names.contains(function_name);
}

/// The inputs (columns of the reading step) that the output `name` of the expressions depends on.
static std::optional<NameSet> getRequiredColumns(const std::optional<ActionsDAG> & dag, const String & name)
{
    if (!dag)
        return NameSet{name};

    const auto * output = dag->tryFindInOutputs(name);
    if (!output)
        return {};

    NameSet columns;
    std::vector<const ActionsDAG::Node *> stack{output};
    std::unordered_set<const ActionsDAG::Node *> visited;
    while (!stack.empty())
    {
        const auto * node = stack.back();
        stack.pop_back();
        if (!visited.insert(node).second)
            continue;
        if (node->type == ActionsDAG::ActionType::INPUT)
            columns.insert(node->result_name);
        for (const auto * child : node->children)
            stack.push_back(child);
    }
    return columns;
}

/// Whether the output `name` of the expressions is a deterministic function of `columns`.
static bool outputDependsOnlyOn(const std::optional<ActionsDAG> & dag, const String & name, const NameSet & columns)
{
    if (!dag)
        return columns.contains(name);
    const auto * output = dag->tryFindInOutputs(name);
    return output && dependsOnlyOn(output, columns);
}

/// `input_node` is the input of an aggregation (`keys`, `aggregates`) or of a `DISTINCT` (`keys`, no aggregates).
static void tryApply(QueryPlan::Node & input_node, const Names & keys, const AggregateDescriptions & aggregates)
{
    /// The expressions and filters between the aggregation and the reading step: only `ExpressionStep` and
    /// `FilterStep`, plus `PREWHERE` and row policies of the reading step.
    QueryDAG query;
    if (!query.build(input_node))
        return;

    QueryPlan::Node * node = &input_node;
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

    auto sorting_key_position = [&](const std::optional<String> & column) -> std::optional<size_t>
    {
        if (!column)
            return {};
        auto it = std::find(sorting_key.column_names.begin(), sorting_key.column_names.end(), *column);
        if (it == sorting_key.column_names.end())
            return {};
        return it - sorting_key.column_names.begin();
    };

    /// The argument of `max`/`min` or the second argument of `argMax`/`argMin`, if it is a column of the reading step.
    auto order_column = [&](const AggregateDescription & aggregate) -> std::optional<String>
    {
        const String name = aggregate.function->getName();
        if ((name == "max" || name == "min") && aggregate.argument_names.size() == 1)
            return mapToReadingColumn(query.dag, aggregate.argument_names[0]);
        if ((name == "argMax" || name == "argMin") && aggregate.argument_names.size() == 2)
            return mapToReadingColumn(query.dag, aggregate.argument_names[1]);
        return {};
    };

    /// The aggregated column `k` is the sorting key column furthest in the sorting key among the arguments of `max`,
    /// `min`, `argMax` and `argMin`. `min` and `max` of earlier columns depend only on the runs that are present.
    std::optional<size_t> aggregated_position;
    for (const auto & aggregate : aggregates)
        if (auto position = sorting_key_position(order_column(aggregate)))
            aggregated_position = std::max(aggregated_position.value_or(0), *position);

    /// If a key or the argument of another aggregate needs that column or a later one (e.g. `min(k2)` with
    /// `uniqExact(k2)`), there is no aggregated column: `min` and `max` then also depend only on the runs that are
    /// present, with a longer prefix. `argMin` and `argMax` need the aggregated column.
    if (aggregated_position)
    {
        const NameSet columns_before(sorting_key.column_names.begin(), sorting_key.column_names.begin() + *aggregated_position);
        bool fits = std::ranges::all_of(keys, [&](const String & key) { return outputDependsOnlyOn(query.dag, key, columns_before); });
        bool has_arg_function = false;
        for (const auto & aggregate : aggregates)
        {
            const String name = aggregate.function->getName();
            has_arg_function = has_arg_function || name == "argMin" || name == "argMax";
            if (sorting_key_position(order_column(aggregate)) == aggregated_position)
                continue;
            for (const auto & argument : aggregate.argument_names)
                fits = fits && outputDependsOnlyOn(query.dag, argument, columns_before);
        }
        if (!fits)
        {
            if (has_arg_function)
                return;
            aggregated_position.reset();
        }
    }

    /// Whether the last row of each run (`max`, `argMax`) or the first one (`min`, `argMin`) is needed. Without
    /// them, only the presence of every run matters, which keeping the last granule of each run guarantees.
    bool need_group_end = false;
    bool need_group_start = false;
    Names set_arguments;
    for (const auto & aggregate : aggregates)
    {
        const String name = aggregate.function->getName();
        if (aggregated_position && sorting_key_position(order_column(aggregate)) == aggregated_position)
        {
            if (name == "min" || name == "argMin")
                need_group_start = true;
            else
                need_group_end = true;
        }
        else if (dependsOnlyOnDistinctValues(name))
        {
            set_arguments.insert(set_arguments.end(), aggregate.argument_names.begin(), aggregate.argument_names.end());
        }
        else
            return;
    }

    /// The prefix: the sorting key columns before the aggregated one or, without it, the shortest sorting key prefix
    /// that covers all columns of the keys and of the other aggregates' arguments.
    size_t prefix_size = 0;
    if (aggregated_position)
    {
        prefix_size = *aggregated_position;
    }
    else
    {
        need_group_end = true;
        Names outputs = keys;
        outputs.insert(outputs.end(), set_arguments.begin(), set_arguments.end());
        for (const auto & output : outputs)
        {
            auto columns = getRequiredColumns(query.dag, output);
            if (!columns)
                return;
            for (const auto & column : *columns)
            {
                auto position = sorting_key_position(column);
                if (!position)
                    return;
                prefix_size = std::max(prefix_size, *position + 1);
            }
        }
    }

    if (primary_key.column_names.size() < prefix_size)
        return;

    /// The prefix columns and the aggregated column must be sorted ascending, with types whose order the aggregation shares.
    const size_t checked_columns = prefix_size + (aggregated_position ? 1 : 0);
    for (size_t i = 0; i < checked_columns; ++i)
    {
        if (i < sorting_key.reverse_flags.size() && sorting_key.reverse_flags[i])
            return;
        if (!isSupportedKeyTypeRecursive(sorting_key.data_types[i]))
            return;
    }

    /// Every key and every argument of the other aggregates must be a deterministic function of the prefix columns
    /// (usually one of them), so that each group is a union of whole runs.
    const NameSet prefix_columns(sorting_key.column_names.begin(), sorting_key.column_names.begin() + prefix_size);
    for (const auto & key : keys)
        if (!outputDependsOnlyOn(query.dag, key, prefix_columns))
            return;
    for (const auto & argument : set_arguments)
        if (!outputDependsOnlyOn(query.dag, argument, prefix_columns))
            return;

    /// Split the filter: conjuncts on the prefix columns only remove whole runs; the other ones must hold for
    /// the witness rows, which is checked with an exact `KeyCondition` on the primary index.
    ActionsDAG::NodeRawConstPtrs witness_conjuncts;
    if (query.filter_node)
    {
        const auto * predicate = query.filter_node;
        while (predicate->type == ActionsDAG::ActionType::ALIAS)
            predicate = predicate->children.front();
        for (const auto * conjunct : ActionsDAG::extractConjunctionAtoms(predicate))
            if (!dependsOnlyOn(conjunct, prefix_columns))
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
        .description = !aggregated_position ? "Skip granules whose groups are all present in later granules"
            : need_group_start              ? "Skip granules that cannot contain the minimum or maximum of the aggregated sorting key column"
                                            : "Skip granules that cannot contain the maximum of the aggregated sorting key column",
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

        if (node->children.size() != 1)
            continue;

        if (auto * aggregating = typeid_cast<AggregatingStep *>(node->step.get()))
        {
            const auto & params = aggregating->getParams();
            if (!aggregating->isGroupingSets() && !params.only_merge && !params.overflow_row)
                tryApply(*node->children.front(), params.keys, params.aggregates);
        }
        else if (auto * distinct = typeid_cast<DistinctStep *>(node->step.get()))
        {
            /// The lowest `DISTINCT` above the reading step: a final `DISTINCT` above a preliminary one has a
            /// `DistinctStep` below it, so `QueryDAG` rejects it.
            tryApply(*node->children.front(), distinct->getColumnNames(), {});
        }
    }
}

}
