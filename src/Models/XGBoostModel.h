#pragma once

#include <Models/IModel.h>
#include <xgboost/c_api.h>

#include <mutex>
#include <string>
#include <vector>

namespace DB
{

/// Extreme Gradient Boosting.
///
/// NOTE: batches are currently staged in memory.
class XGBoostModel : public IModel
{
public:
    XGBoostModel() = default;
    ~XGBoostModel() override;

protected:
    void setHyperParameters(const HyperParameters & hyper_parameters) override;

    void startTrainingImpl(const Block & header, const String & target_column_) override;
    void addTrainingDataImpl(const Block & batch) override;
    void finalizeTrainingImpl() override;
    ColumnPtr predictImpl(const Block & batch) override;

private:
    BoosterHandle booster {nullptr};
    DMatrixHandle dmatrix {nullptr};

    HyperParameters hps;

    String target_column;
    std::vector<String> feature_columns;
    std::size_t n_features = 0;
    std::size_t n_rows = 0;

    /// Keeps the features in a flattened vector
    std::vector<float> flattened_features;
    /// Vector of labels
    std::vector<float> labels;

    /// A trained model is shared through the ModelRegistry, so concurrent
    /// PREDICT queries would otherwise race inside XGBoosterPredict, which
    /// writes into a prediction buffer owned by the booster.
    std::mutex predict_mutex;
};

}
