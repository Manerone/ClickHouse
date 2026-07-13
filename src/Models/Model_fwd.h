#pragma once

#include <memory>
#include <base/types.h>

namespace DB
{
using HyperParameters = String;
using PredictParameters = String;

class IModel;
using ModelPtr = std::shared_ptr<IModel>;
}
