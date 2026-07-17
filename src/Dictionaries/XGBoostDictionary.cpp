#include <Dictionaries/XGBoostDictionary.h>

#include <Core/Block.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/IDataType.h>
#include <Dictionaries/DictionaryFactory.h>
#include <Dictionaries/DictionaryPipelineExecutor.h>
#include <Interpreters/castColumn.h>
#include <Dictionaries/XGBoostModel.h>
#include <QueryPipeline/BlockIO.h>
#include <QueryPipeline/Pipe.h>
#include <Common/logger_useful.h>

#include <Poco/JSON/Object.h>
#include <Poco/Util/AbstractConfiguration.h>

#include <sstream>


namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int UNSUPPORTED_METHOD;
}


XGBoostDictionary::XGBoostDictionary(
    const StorageID & dict_id_, const DictionaryStructure & dict_struct_, DictionarySourcePtr source_ptr_, Configuration configuration_)
    : IDictionary(dict_id_)
    , dict_struct(dict_struct_)
    , source_ptr(std::move(source_ptr_))
    , configuration(std::move(configuration_))
    , log(getLogger("XGBoostDictionary"))
{
    loadData();
}


void XGBoostDictionary::loadData()
{
    /// Build the training header: features columns in declaration order, followed by the single
    /// target attribute. The XGBoost backend treats every column other than the target as a feature, so the
    /// feature order is the key-column declaration order.
    ColumnsWithTypeAndName header_columns;
    for (const auto & key_attribute : *dict_struct.key)
        header_columns.emplace_back(key_attribute.type->createColumn(), key_attribute.type, key_attribute.name);
    const auto & target_attribute = dict_struct.getAttribute(configuration.target_name);
    header_columns.emplace_back(target_attribute.type->createColumn(), target_attribute.type, target_attribute.name);
    Block header(header_columns);

    model = std::make_unique<XGBoostModel>(configuration.hyper_parameters);
    model->startTraining(header, configuration.target_name);

    BlockIO io = source_ptr->loadAll();

    /// Stream the source rows into the model block by block, so the whole dataset never has to be
    /// materialized at once. The source delivers the columns cast to the declared types; the backend reads
    /// them as floats.
    io.executeWithCallbacks(
        [&]()
        {
            DictionaryPipelineExecutor executor(io.pipeline, false);
            io.pipeline.setConcurrencyControl(false);

            Block block;
            while (executor.pull(block))
                model->addTrainingData(block);
        });

    model->finalizeTraining();

    element_count = model->getFeatureNames().size();

    LOG_INFO(log, "Loaded XGBoost dictionary trained on {} feature(s)", element_count);
}


const std::vector<String> & XGBoostDictionary::getFeatureNames() const
{
    return model->getFeatureNames();
}


ColumnPtr XGBoostDictionary::predict(const Block & features, const PredictParameters & params) const
{
    return model->predict(features, params);
}


ColumnPtr XGBoostDictionary::getColumn(
    const std::string & attribute_name,
    const DataTypePtr & attribute_type,
    const Columns & key_columns,
    const DataTypes & key_types,
    DefaultOrFilter default_or_filter) const
{
    /// Only the target attribute is computable (it is the prediction). The features are the key, not an
    /// attribute, so there is nothing else to query.
    if (attribute_name != configuration.target_name)
        throw Exception(
            ErrorCodes::UNSUPPORTED_METHOD,
            "XGBoost dictionary only supports querying the target attribute '{}' (the prediction), got '{}'",
            configuration.target_name,
            attribute_name);

    dict_struct.validateKeyTypes(key_types);

    const auto & feature_names = getFeatureNames();
    const size_t rows = key_columns.empty() ? 0 : key_columns.front()->size();

    /// The predicted value is always available, so mark every row as resolved for the short-circuit path.
    if (std::holds_alternative<RefFilter>(default_or_filter))
        std::get<RefFilter>(default_or_filter).get().assign(rows, static_cast<UInt8>(0));

    /// Bind each key column to the corresponding feature name (both are in declaration order).
    Block features;
    for (size_t i = 0; i < feature_names.size(); ++i)
        features.insert({key_columns[i], (*dict_struct.key)[i].type, feature_names[i]});

    query_count.fetch_add(rows, std::memory_order_relaxed);

    ColumnPtr predictions = predict(features, {});

    /// `dictGet` must return the declared attribute type; predictions are Float64, so cast to it.
    return castColumn({predictions, std::make_shared<DataTypeFloat64>(), ""}, attribute_type);
}


ColumnUInt8::Ptr XGBoostDictionary::hasKeys(const Columns & key_columns, const DataTypes & key_types) const
{
    dict_struct.validateKeyTypes(key_types);

    /// Any feature vector can be predicted, so every key is considered present.
    const size_t rows = key_columns.front()->size();
    auto result = ColumnUInt8::create(rows);
    result->getData().assign(rows, static_cast<UInt8>(1));

    query_count.fetch_add(rows, std::memory_order_relaxed);

    return result;
}


Pipe XGBoostDictionary::read(const Names &, size_t, size_t) const
{
    throw Exception(
        ErrorCodes::UNSUPPORTED_METHOD, "An XGBoost dictionary trains a model and cannot be read back as a table of rows");
}


void registerDictionaryXGBoost(DictionaryFactory & factory);
void registerDictionaryXGBoost(DictionaryFactory & factory)
{
    auto create_layout = [](const std::string & /* full_name */,
                            const DictionaryStructure & dict_struct,
                            const Poco::Util::AbstractConfiguration & config,
                            const std::string & config_prefix,
                            DictionarySourcePtr source_ptr,
                            ContextPtr /* global_context */,
                            bool /* created_from_ddl */) -> DictionaryPtr
    {
        /// The structure must be a complex key of one or more numeric feature columns, followed by exactly one
        /// numeric attribute: the training target.
        if (!dict_struct.key || dict_struct.key->empty())
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS, "XGBoost dictionary must have at least one key column (the numeric features)");

        auto validate_numeric = [](const std::string & role, const DictionaryAttribute & attribute)
        {
            const WhichDataType which(attribute.type);
            if (!which.isNativeNumber() || which.isEnum())
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "XGBoost dictionary {} '{}' must be a native numeric type, got {}",
                    role,
                    attribute.name,
                    attribute.type->getName());
        };

        for (const auto & key_attribute : *dict_struct.key)
            validate_numeric("feature key", key_attribute);

        if (dict_struct.attributes.size() != 1)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "XGBoost dictionary must have exactly one attribute (the training target), got {}",
                dict_struct.attributes.size());

        const auto & target_attribute = dict_struct.attributes[0];
        validate_numeric("target attribute", target_attribute);

        const String layout_prefix = config_prefix + ".layout.xgboost";

        /// `target` is required and must name the single attribute. Every other layout parameter is an XGBoost
        /// hyperparameter; the hyperparameters are collected into a JSON object and validated against the
        /// backend's allowlist when the model trains (see XGBoostModel).
        if (!config.has(layout_prefix + ".target"))
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "XGBoost dictionary layout requires a 'target' parameter naming the target attribute, e.g. target '{}'",
                target_attribute.name);

        Poco::Util::AbstractConfiguration::Keys layout_keys;
        config.keys(layout_prefix, layout_keys);

        Poco::JSON::Object hyper_json;
        for (const auto & key : layout_keys)
        {
            const String value = config.getString(layout_prefix + "." + key);
            if (key == "target")
            {
                if (value != target_attribute.name)
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "XGBoost dictionary: 'target' names '{}', but the only attribute is '{}'",
                        value,
                        target_attribute.name);
                continue;
            }
            hyper_json.set(key, value);
        }

        std::ostringstream oss;
        hyper_json.stringify(oss);

        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

        XGBoostDictionary::Configuration cfg{
            .target_name = target_attribute.name,
            .hyper_parameters = oss.str(),
            .dict_lifetime = dict_lifetime,
        };

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

        return std::make_unique<XGBoostDictionary>(dict_id, dict_struct, std::move(source_ptr), std::move(cfg));
    };

    factory.registerLayout(
        "xgboost",
        create_layout,
        /* is_layout_complex= */ true,
        /* has_layout_complex= */ false,
        Documentation{
            .description = "A computational dictionary that trains an immutable XGBoost model at load time from a source table of "
                           "`(features..., target)` rows, then predicts the target for a feature vector through `dictGet` or the "
                           "`predictXGBoost` function. The feature columns are the key and the single attribute is the target.",
            .syntax = "LAYOUT(XGBOOST(target 'name' [objective '...'] [num_iterations N] [max_depth N] [eta 0.3] [...]))",
            .introduced_in = {26, 7}});
}

}
