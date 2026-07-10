#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <base/types.h>
#include <base/strong_typedef.h>
#include <Core/Types_fwd.h>

namespace DB
{
using HyperParameters = std::unordered_map<String, String>;

class IModel;
using ModelPtr = std::shared_ptr<IModel>;
}
