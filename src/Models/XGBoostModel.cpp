#include <Core/ColumnWithTypeAndName.h>
#include <DataTypes/IDataType.h>
#include <Models/XGBoostModel.h>

#include <Common/Exception.h>
#include <Common/scope_guard_safe.h>
#include <Core/Block.h>
#include <Columns/IColumn.h>
#include <Columns/ColumnsNumber.h>

#include <base/types.h>
#include <xgboost/c_api.h>

#include <fmt/format.h>

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

    // Sets the parameters provided by the user
    // TODO: dont accept everything the user provided
    for (const auto & [key, value] : hps){
        throwOnError(XGBoosterSetParam(booster, key.c_str(), value.c_str()));
    }

    int n_iter = 100;
    if (auto it = hps.find("num_iterations"); it != hps.end())
        n_iter = std::stoi(it->second);
    if (auto it = hps.find("n_estimators"); it != hps.end())
        n_iter = std::stoi(it->second);

    // Train the model
    for (int i = 0; i < n_iter; ++i){
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

ColumnPtr XGBoostModel::predictImpl(const Block & batch)
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

        char const config[] =
        "{\"training\": false, \"type\": 0, "
        "\"iteration_begin\": 0, \"iteration_end\": 0, \"strict_shape\": false}";

        throwOnError(
            XGBoosterPredictFromDMatrix(booster, predict_dmatrix, config, &out_shape, &out_dim, &out_result)
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
}
