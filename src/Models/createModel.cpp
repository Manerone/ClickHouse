#include <Models/createModel.h>

#include <Common/Exception.h>

#include <Models/XGBoostModel.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int MODEL_NOT_FOUND;
}

ModelPtr createModel(const String& algorithm, const HyperParameters& hyper_parameters)
{
    ModelPtr model;

    if (algorithm == "xgboost")
    {
        model = std::make_shared<XGBoostModel>();
    }
    else
    {
        throw Exception(ErrorCodes::MODEL_NOT_FOUND, "Unknown model algorithm: {}", algorithm);
    }

    model->setHyperParameters(hyper_parameters);
    return model;
}

}
