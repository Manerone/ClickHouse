#include <Core/ColumnWithTypeAndName.h>
#include <DataTypes/IDataType.h>
#include <IO/ReadHelpers.h>
#include <Models/Model_fwd.h>
#include <Models/XGBoostModel.h>

#include <Common/Exception.h>
#include <Common/scope_guard_safe.h>
#include <IO/ReadHelpers.h>
#include <Core/Block.h>
#include <Columns/IColumn.h>
#include <Columns/ColumnsNumber.h>

#include <base/types.h>
#include <xgboost/c_api.h>

#include <fmt/format.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <unordered_map>
#include <unordered_set>

namespace DB
{

namespace ErrorCodes
{
    extern const int XGBOOST_ERROR;
}

namespace
{

inline void throwOnError(int err){
    if(err != 0){
        std::string what{XGBGetLastError()};
        throw Exception(ErrorCodes::XGBOOST_ERROR, "Error: {}", what); \
    }
}
}

XGBoostModel::~XGBoostModel()
{
    if (booster)
        XGBoosterFree(booster);
    if (dmatrix)
        XGDMatrixFree(dmatrix);
}

void XGBoostModel::setHyperParameters(const HyperParameters & parameters)
{
    hps = parameters;
}

void XGBoostModel::throwIfTypeIsInvalid(const ColumnWithTypeAndName &col){
    auto type = col.type;
    WhichDataType which(type->getTypeId());
    if(!which.isNativeNumber() || which.isEnum()){
        throw Exception(
            ErrorCodes::XGBOOST_ERROR, "XGBoost only accepts numerical types. The column {} has type {}", col.column->getName(), type->getName());
    }
}

void XGBoostModel::startTrainingImpl(const Block & header, const String & target_column_)
{
    if (!header.has(target_column_))
        throw Exception(
            ErrorCodes::XGBOOST_ERROR,
            "Target column '{}' is not present in the training data",
            target_column_);

    target_column = target_column_;

    feature_columns.clear();
    for (const auto & column : header.getColumnsWithTypeAndName())
        if (column.name != target_column)
            feature_columns.push_back(column.name);

    if (feature_columns.empty())
        throw Exception(
            ErrorCodes::XGBOOST_ERROR,
            "No feature columns for training (target column is '{}')",
            target_column);

    n_features = feature_columns.size();
}

void XGBoostModel::addTrainingDataImpl(const Block & batch)
{
    const std::size_t rows = batch.rows();
    if (rows == 0)
        return;

    std::vector<const IColumn *> feature_cols;
    feature_cols.reserve(n_features);
    for (const auto & name : feature_columns){
        const auto &col_with_type_and_name = batch.getByName(name);
        const auto *icol = col_with_type_and_name.column.get();
        feature_cols.push_back(icol);

        throwIfTypeIsInvalid(col_with_type_and_name);
    }

    const IColumn * label_col {nullptr};
    {
        const auto &col_with_type_and_name = batch.getByName(target_column);
        label_col = col_with_type_and_name.column.get();
        throwIfTypeIsInvalid(col_with_type_and_name);
    }
    assert(label_col);

    flattened_features.reserve(ingested_rows + (rows * n_features));
    labels.reserve(ingested_rows + rows);
    
    // Transforms from the Block into a flattened vector, stores the tuples row-wise
    for (std::size_t r = 0; r < rows; ++r)
    {
        for (std::size_t c = 0; c < n_features; ++c)
            flattened_features.push_back(static_cast<float>(feature_cols[c]->getFloat64(r)));
        labels.push_back(static_cast<float>(label_col->getFloat64(r)));
    }

    ingested_rows += rows;
}

void XGBoostModel::finalizeTrainingImpl()
{
    if (ingested_rows == 0)
        throw Exception(ErrorCodes::XGBOOST_ERROR, "No training data was provided");


    assert(labels.size() == ingested_rows);
    assert(flattened_features.size() == ingested_rows * n_features);

    // Create the DMatrix
    throwOnError(XGDMatrixCreateFromMat(flattened_features.data(), ingested_rows, n_features, 0, &dmatrix));

    // Create the model
    throwOnError(XGBoosterCreate(&dmatrix, 1, &booster));

    // Set the label for each row
    throwOnError(XGDMatrixSetFloatInfo(dmatrix, "label", labels.data(), ingested_rows));

    // Validate the parameters provided by the user
    const auto params = sanitizeTrainingParams(hps);

    for (const auto & [key, value] : params){
        throwOnError(XGBoosterSetParam(booster, key.c_str(), value.c_str()));
    }

    // Train the model
    for (int i = 0; i < num_iterations; ++i){
        throwOnError(XGBoosterUpdateOneIter(booster, i, dmatrix));
    }

    /// Release ingestion resources; the booster is self-contained from here on.
    XGDMatrixFree(dmatrix);
    dmatrix = nullptr;

    flattened_features.clear();
    flattened_features.shrink_to_fit();
    labels.clear();
    labels.shrink_to_fit();
}

ColumnPtr XGBoostModel::predictImpl(const Block & batch, const PredictParameters & params)
{
    if(batch.columns() != n_features){
        throw Exception(
            ErrorCodes::XGBOOST_ERROR,
            "Expected {} features, got {}", n_features, batch.columns());
    }

    const std::size_t rows = batch.rows();
    if (rows == 0)
        return ColumnFloat64::create();

    std::vector<const IColumn *> feature_cols;
    feature_cols.reserve(n_features);
    for (const auto & name : feature_columns)
        feature_cols.push_back(batch.getByName(name).column.get());

    std::vector<float> features;

    features.reserve(rows * n_features);

    for (std::size_t r = 0; r < rows; ++r)
    {
        for (std::size_t c = 0; c < n_features; ++c)
            features.push_back(static_cast<float>(feature_cols[c]->getFloat64(r)));
    }

    DMatrixHandle predict_dmatrix {nullptr};
    SCOPE_EXIT({
        if (predict_dmatrix)
        {
            XGDMatrixFree(predict_dmatrix);
        }
    });

    throwOnError(XGDMatrixCreateFromMat(features.data(), rows, n_features, 0, &predict_dmatrix));

    auto result = ColumnFloat64::create();

    {
        std::lock_guard lock(predict_mutex);

        /* Shape of output prediction */
        bst_ulong const * out_shape{nullptr};
        /* Dimension of output prediction */
        bst_ulong out_dim {0};
        /* Pointer to a thread local contiguous array, assigned in prediction function. */
        float const * out_result{nullptr};

        String config = sanitizePredictParams(params);

        throwOnError(
            XGBoosterPredictFromDMatrix(booster, predict_dmatrix, config.c_str(), &out_shape, &out_dim, &out_result)
        );

        size_t out_len = 1;
        for (uint64_t i = 0; i < out_dim; ++i)
            out_len *= out_shape[i];

        // Should have predicted the number of inputted rows
        assert(rows == out_len);

        auto & data = result->getData();
        data.resize(out_len);
        for (std::size_t i = 0; i < out_len; ++i)
            data[i] = static_cast<Float64>(out_result[i]);
    }

    return result;
}

std::unordered_map<String, String> XGBoostModel::sanitizeTrainingParams(const HyperParameters & params)
{
    std::unordered_map<String, String> sanitized;

    if (params.empty())
        return sanitized;

    static const std::unordered_set<String> allowed_keys{
        "booster",
        "objective",
        "eval_metric",
        "seed",
        "verbosity",
        "nthread",
        "eta",
        "learning_rate",
        "gamma",
        "max_depth",
        "min_child_weight",
        "max_delta_step",
        "subsample",
        "sampling_method",
        "colsample_bytree",
        "colsample_bylevel",
        "colsample_bynode",
        "lambda",
        "reg_lambda",
        "alpha",
        "reg_alpha",
        "tree_method",
        "scale_pos_weight",
        "grow_policy",
        "max_leaves",
        "max_bin",
        "num_parallel_tree",
        "num_iterations"};

    Poco::JSON::Parser parser;
    Poco::Dynamic::Var parsed;
    try
    {
        parsed = parser.parse(params);
    }
    catch (const Poco::Exception & e)
    {
        throw Exception(
            ErrorCodes::XGBOOST_ERROR, "Training parameters are not valid JSON: {}", e.displayText());
    }

    const auto & object = parsed.extract<Poco::JSON::Object::Ptr>();
    if (object.isNull())
        throw Exception(ErrorCodes::XGBOOST_ERROR, "Training parameters must be a JSON object");

    for (const auto & [key, value] : *object)
    {
        if (!allowed_keys.contains(key))
            throw Exception(
                ErrorCodes::XGBOOST_ERROR, "Unknown or forbidden training parameter '{}'", key);
        // If we found num_iterations, record this value and do not add it to the final map
        if (key == "num_iterations")
        {
            int parsed_iterations = 0;
            if (!tryParse(parsed_iterations, value.toString()) || parsed_iterations <= 0)
                throw Exception(
                    ErrorCodes::XGBOOST_ERROR, "Parameter 'num_iterations' must be a positive integer, got '{}'", value.toString());
            num_iterations = parsed_iterations;
        }
        else
        {
            sanitized.emplace(key, value.toString());
        }
    }

    return sanitized;
}

String XGBoostModel::sanitizePredictParams(const PredictParameters & params)
{
    // Default parameters
    Poco::JSON::Object config;
    config.set("type", 0);
    config.set("iteration_begin", 0);
    config.set("iteration_end", 0);
    config.set("strict_shape", false);
    config.set("training", false);

    if (!params.empty())
    {
        static const std::unordered_set<String> allowed_keys{
            "type", "iteration_begin", "iteration_end", "strict_shape", "ntree_limit"};

        Poco::JSON::Parser parser;
        Poco::Dynamic::Var parsed;
        try
        {
            parsed = parser.parse(params);
        }
        catch (const Poco::Exception & e)
        {
            throw Exception(
                ErrorCodes::XGBOOST_ERROR, "Prediction parameters are not valid JSON: {}", e.displayText());
        }

        const auto & object = parsed.extract<Poco::JSON::Object::Ptr>();
        if (object.isNull())
            throw Exception(ErrorCodes::XGBOOST_ERROR, "Prediction parameters must be a JSON object");

        for (const auto & [key, value] : *object)
        {
            if (!allowed_keys.contains(key))
                throw Exception(
                    ErrorCodes::XGBOOST_ERROR, "Unknown or forbidden prediction parameter '{}'", key);
            config.set(key, value);
        }
    }

    std::ostringstream oss;
    config.stringify(oss);
    return oss.str();
}
}
