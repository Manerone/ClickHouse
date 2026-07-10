#include <TableFunctions/TableFunctionPredict.h>

#include <Parsers/ASTPredictQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Analyzer/TableFunctionNode.h>
#include <DataTypes/DataTypeString.h>
#include <Storages/StorageMemory.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/registerTableFunctions.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Interpreters/executeQuery.h>
#include <Interpreters/Context.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Models/ModelRegistry.h>
#include <Columns/ColumnsNumber.h>
#include <Storages/StorageValues.h>
#include <DataTypes/DataTypesNumber.h>

namespace DB
{


namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int BAD_ARGUMENTS;
}

VectorWithMemoryTracking<size_t> TableFunctionPredict::skipAnalysisForArguments(const QueryTreeNodePtr & query_node_table_function, ContextPtr) const
{
    const auto & table_function_node = query_node_table_function->as<TableFunctionNode &>();
    const auto & arguments = table_function_node.getArguments().getNodes();

    VectorWithMemoryTracking<size_t> result;
    result.reserve(arguments.size());
    for (size_t i = 0; i < arguments.size(); ++i)
        result.push_back(i);
    return result;
}

void TableFunctionPredict::parseArguments(const ASTPtr & ast_function, ContextPtr)
{
    ASTs & args_func = ast_function->children;

    if (args_func.size() != 1)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Table function '{}' must have arguments", getName());

    const ASTs & args = args_func[0]->children;

    /// Two shapes reach here: the AST produced directly by the parser wraps the model and table
    /// names in a single ASTPredictQuery node; the AST reconstructed by the analyzer from the
    /// query tree carries them as two plain identifier arguments.
    if (args.size() == 1)
    {
        const auto * predict = args[0]->as<ASTPredictQuery>();
        if (!predict)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Table function '{}' expects 'MODEL model, TABLE table' arguments", getName());

        model_name = predict->model_name->as<ASTIdentifier>()->name();
        table_name = predict->table_name->as<ASTIdentifier>()->name();
    }
    else if (args.size() == 2)
    {
        const auto * model_id = args[0]->as<ASTIdentifier>();
        const auto * table_id = args[1]->as<ASTIdentifier>();
        if (!model_id || !table_id)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Table function '{}' expects 'MODEL model, TABLE table' arguments", getName());

        model_name = model_id->name();
        table_name = table_id->name();
    }
    else
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Table function '{}' must have arguments", getName());
}

StoragePtr TableFunctionPredict::executeImpl(
    const ASTPtr & /*ast_function*/,
    ContextPtr context,
    const std::string & tf_table_name,
    ColumnsDescription /*cached_columns*/,
    bool) const
{
    StorageID table_id{context->getCurrentDatabase(), table_name};
    StoragePtr storage = DatabaseCatalog::instance().getTable(table_id, context);

    /// Run the input query on a separate context with its own query id, so it
    /// does not collide in the process list with the outer query that invoked
    /// the predict table function (which would otherwise raise
    /// QUERY_WITH_SAME_ID_IS_ALREADY_RUNNING when a client-supplied query id is
    /// in effect).
    auto ctx = Context::createCopy(context);
    ctx->setCurrentQueryId("");
    auto io = executeQuery("SELECT * FROM " + table_name, ctx, QueryFlags{ .internal = true }, QueryProcessingStage::Complete).second;
    PullingPipelineExecutor executor(io.pipeline);

    auto model = ModelRegistry::instance().getModel(model_name);

    // Stream blocks through the model, accumulating the per-block predictions.
    auto col = ColumnFloat64::create();

    Block block;
    while (executor.pull(block))
    {
        if (block.rows() == 0)
            continue;
        ColumnPtr batch_predictions = model->predict(block);
        col->insertRangeFrom(*batch_predictions, 0, batch_predictions->size());
    }

    Block result_block;
    result_block.insert({std::move(col),
                        std::make_shared<DataTypeFloat64>(),
                        "prediction"});

    ColumnsDescription descr{{{"prediction",
                            std::make_shared<DataTypeFloat64>()}}};

    auto storage_pred = std::make_shared<StorageValues>(
        StorageID{context->getCurrentDatabase(), tf_table_name},
        descr,
        result_block);

    storage_pred->startup();
    return storage_pred;
}

void registerTableFunctionPredict(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionPredict>(
        FunctionDocumentation{
            .description = "Runs model's predictions on the input table",
            .returned_value = {"A table containing a single row - predicted target values of the model"},
            .category = FunctionDocumentation::Category::TableFunction},
        TableFunctionProperties{.allow_readonly = true});
}

}
