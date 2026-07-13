#include <Interpreters/InterpreterCreateModelQuery.h>
#include <Interpreters/InterpreterFactory.h>
#include <Parsers/ASTCreateModelQuery.h>
#include <Access/ContextAccess.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Storages/IStorage.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Interpreters/executeQuery.h>
#include <Columns/IColumn.h>

#include <Models/ModelRegistry.h>
#include <Models/createModel.h>

namespace DB
{


namespace ErrorCodes
{
    extern const int THERE_IS_NO_COLUMN;
    extern const int UNKNOWN_TABLE;
    extern const int BAD_TYPE_OF_FIELD;
}

BlockIO InterpreterCreateModelQuery::execute()
{
    auto context = getContext();

    const auto & create_model_query = query_ptr->as<const ASTCreateModelQuery &>();

    // TODO: access
    // current_context->checkAccess(AccessType::CREATE_NAMED_COLLECTION, query.collection_name);

    const String model_name = create_model_query.model_name->as<ASTIdentifier>()->name();
    const String algorithm = create_model_query.algorithm->as<ASTLiteral>()->value.safeGet<String>();
    const HyperParameters hyper_parameters = create_model_query.options->as<ASTLiteral>()->value.safeGet<String>();
    const String target_name = create_model_query.target->as<ASTLiteral>()->value.safeGet<String>();
    const String table_name = create_model_query.table_name->as<ASTIdentifier>()->name();

    {
        StorageID table_id{context->getCurrentDatabase(), table_name};
        StoragePtr storage = DatabaseCatalog::instance().getTable(table_id, context);

        const auto metadata_snapshot = storage->getInMemoryMetadataPtr(context, false);

        if (!metadata_snapshot->columns.hasPhysical(target_name))
            throw Exception(
                ErrorCodes::THERE_IS_NO_COLUMN,
                "Model {} requires target column {} which does not exist in the table {}",
                model_name, target_name, table_name
            );
    }

    /// Run the training query on a separate context with its own query id, so
    /// it does not collide in the process list with the outer CREATE MODEL query
    /// (which would otherwise raise QUERY_WITH_SAME_ID_IS_ALREADY_RUNNING when a
    /// client-supplied query id is in effect).
    auto training_context = Context::createCopy(context);
    training_context->setCurrentQueryId("");

    auto io = executeQuery("SELECT * FROM " + table_name, training_context, QueryFlags{ .internal = true }, QueryProcessingStage::Complete).second;
    PullingPipelineExecutor executor(io.pipeline);

    // Create the model, then stream the training data into it block by block so
    // the whole dataset never has to be materialized in memory at once.
    ModelPtr model = createModel(algorithm, hyper_parameters);
    model->startTraining(executor.getHeader(), target_name);

    Block block;
    while (executor.pull(block))
        model->addTrainingData(block);

    model->finalizeTraining();

    ModelRegistry::instance().registerModel(
        model_name,
        model
    );

    return {};
}

void registerInterpreterCreateModelQuery(InterpreterFactory & factory);
void registerInterpreterCreateModelQuery(InterpreterFactory & factory)
{
    auto create_fn = [] (const InterpreterFactory::Arguments & args)
    {
        return std::make_unique<InterpreterCreateModelQuery>(args.query, args.context);
    };
    factory.registerInterpreter("InterpreterCreateModelQuery", create_fn);
}

}
