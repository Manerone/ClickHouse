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
    for (const auto & name : feature_columns)
        feature_cols.push_back(batch.getByName(name).column.get());

    const IColumn & label_col = *batch.getByName(target_column).column;


    flattened_features.reserve(n_rows + (rows * n_features));
    labels.reserve(n_rows + rows);
    

    for (std::size_t r = 0; r < rows; ++r)
    {
        for (std::size_t c = 0; c < n_features; ++c)
            flattened_features.push_back(static_cast<float>(feature_cols[c]->getFloat64(r)));
        labels.push_back(static_cast<float>(label_col.getFloat64(r)));
    }

    n_rows += rows;
}

void XGBoostModel::finalizeTrainingImpl()
{
    if (n_rows == 0)
        throw Exception(ErrorCodes::XGBOOST_ERROR, "No training data was provided");


    assert(labels.size() == n_rows);
    assert(flattened_features.size() == n_rows * n_features);

    throwOnError(XGDMatrixCreateFromMat(flattened_features.data(), n_rows, n_features, 0, &dmatrix));

    throwOnError(XGBoosterCreate(&dmatrix, 1, &booster));

    throwOnError(XGDMatrixSetFloatInfo(dmatrix, "label", labels.data(), n_rows));

    for (const auto & [key, value] : hps){
        throwOnError(XGBoosterSetParam(booster, key.c_str(), value.c_str()));
    }

    int n_iter = 100;
    if (auto it = hps.find("num_iterations"); it != hps.end())
        n_iter = std::stoi(it->second);
    if (auto it = hps.find("n_estimators"); it != hps.end())
        n_iter = std::stoi(it->second);

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
    bst_ulong out_len {0};
    const float* out_result {nullptr};

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
    throwOnError(XGDMatrixCreateFromMat(features.data(), rows, n_features, 0, &predict_dmatrix));
    SCOPE_EXIT({ XGDMatrixFree(predict_dmatrix); });

    auto result = ColumnFloat64::create();

    {
        /// XGBoosterPredict writes into a prediction buffer owned by the booster
        /// and returns a pointer into it, so the call and the copy-out below must
        /// be serialized against concurrent predictions on the same model.
        std::lock_guard lock(predict_mutex);

        throwOnError(XGBoosterPredict(
            booster, predict_dmatrix, 0, 0,
            0, &out_len, &out_result));

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
