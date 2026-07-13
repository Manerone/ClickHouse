#include <Models/IModel.h>
#include <Models/Model_fwd.h>

#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

void IModel::startTraining(const Block & header, const String & target_column)
{
    if (model_state != ModelState::Created)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "startTraining called on a model that is already training or trained");

    startTrainingImpl(header, target_column);
    model_state = ModelState::Training;
}

void IModel::addTrainingData(const Block & batch)
{
    if (model_state != ModelState::Training)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "addTrainingData called before startTraining or after finalizeTraining");

    addTrainingDataImpl(batch);
}

void IModel::finalizeTraining()
{
    if (model_state != ModelState::Training)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "finalizeTraining called on a model that is not training");

    finalizeTrainingImpl();
    model_state = ModelState::Trained;
}

ColumnPtr IModel::predict(const Block & batch, const PredictParameters & params)
{
    if (model_state != ModelState::Trained)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "predict called on a model that has not finished training");

    return predictImpl(batch, params);
}

}
