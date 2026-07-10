#pragma once

#include <Models/Model_fwd.h>
#include <Core/Block_fwd.h>
#include <Columns/IColumn_fwd.h>

namespace DB
{

/// Lifecycle of a model: Created -> Training -> Trained.
/// Enforced by the base class so implementations don't each re-check it.
enum class ModelState
{
    Created,   /// constructed, hyperparameters set, no data yet
    Training,  /// startTraining called; accepting batches
    Trained,   /// finalizeTraining called; ready for predict
};

/// Interface for machine-learning models.
///
/// Training follows an explicit streaming lifecycle so that datasets that do
/// not fit in memory can be fed batch by batch:
///
///     createModel(...) -> startTraining -> addTrainingData* -> finalizeTraining -> predict*
///
/// The public methods below are non-virtual wrappers that validate the current
/// `ModelState` and forward to the protected `*Impl` hooks; each concrete model
/// implements only the hooks. This keeps the state machine in a single place.
class IModel
{
public:
    virtual ~IModel() = default;

    /// Begin a training session. `header` is a sample block describing the
    /// input schema (structure only, no rows); `target_column` names the label
    /// column - every other column in `header` is treated as a feature.
    void startTraining(const Block & header, const String & target_column);

    /// Feed one batch of training rows. Called repeatedly, once per source
    /// block
    void addTrainingData(const Block & batch);

    /// Signal end of training data: build/train the model.
    // After this the model is Trained and ready to predict.
    void finalizeTraining();

    /// Predict on a batch of feature rows. Returns a Float64 column of
    /// predictions with one element per row of `batch`.
    ColumnPtr predict(const Block & batch);

    ModelState state() const { return model_state; }

protected:
    /// Models can be created only via the `createModel` function.
    IModel() = default;

    /// Sets model's hyper parameters, e.g. {"learning_rate":"0.1", "max_depth":"6", ...}.
    /// Called right after the creation of the model in `createModel`.
    virtual void setHyperParameters(const HyperParameters & hyper_parameters) = 0;

    /// Backend hooks - the public methods above wrap these with state
    /// transitions and validation, so each backend implements only the work.
    virtual void startTrainingImpl(const Block & header, const String & target_column) = 0;
    virtual void addTrainingDataImpl(const Block & batch) = 0;
    virtual void finalizeTrainingImpl() = 0;
    virtual ColumnPtr predictImpl(const Block & batch) = 0;

private:
    ModelState model_state = ModelState::Created;

    friend ModelPtr createModel(const String & algorithm, const HyperParameters & hyper_parameters);
};

}
