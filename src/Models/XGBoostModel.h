#pragma once

#include <Models/IModel.h>
#include <Models/Model_fwd.h>
#include <xgboost/c_api.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace DB
{

struct ColumnWithTypeAndName;


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
    ColumnPtr predictImpl(const Block & batch, const PredictParameters & params) override;

private:
    void throwIfTypeIsInvalid(const ColumnWithTypeAndName &col);

    std::unordered_map<String, String> sanitizeTrainingParams(const HyperParameters & params);
    String sanitizePredictParams(const PredictParameters & params);

    BoosterHandle booster {nullptr};
    DMatrixHandle dmatrix {nullptr};

    HyperParameters hps;
    int num_iterations {100};

    /// Target column
    String target_column;
    /// Name of the feature columns
    std::vector<String> feature_columns;
    /// Number of features
    std::size_t n_features = 0;
    /// Number of rows already ingested
    std::size_t ingested_rows = 0;

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
