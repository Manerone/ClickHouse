#include <Columns/ColumnConst.h>
#include <Columns/ColumnString.h>
#include <Core/Block.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Models/ModelRegistry.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int ILLEGAL_COLUMN;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int BAD_ARGUMENTS;
}

/// Row-wise inference: predict(model_name, feature1, feature2, ...[, params_json]).
///
///   SELECT id, predict('my_model', f1, f2, f3) FROM t WHERE ... LIMIT ...
///
/// Features are passed as individual columns, positionally: argument i (after
/// the model name) is bound to the model's i-th training feature, so their
/// order must match the training order. Because the features arrive as separate
/// columns, they are dropped straight into the prediction Block with no
/// array (de)interleaving.
///
/// Feature arguments are numeric; the optional prediction parameters are a JSON
/// String. That type split makes the trailing `params` unambiguous: it is the
/// last argument iff that argument is a String.
///
/// Top-level Nullable/LowCardinality feature columns are unwrapped by the
/// default IFunction machinery (see `useDefaultImplementationForNulls` /
/// `useDefaultImplementationForLowCardinalityColumns`); a NULL feature therefore
/// yields a NULL prediction for that row.
class FunctionPredict final : public IFunction
{
public:
    static constexpr auto name = "predict";

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionPredict>(); }

    String getName() const override { return name; }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    /// A model can be retrained/replaced under the same name, so a call is not pure.
    bool isDeterministic() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return true; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() < 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Function '{}' expects 'model_name, feature1[, feature2, ...][, params]'", getName());

        if (!isString(arguments[0].type))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "First argument of function '{}' (model name) must be a constant String", getName());

        const size_t feature_end = featureEnd(arguments);

        if (feature_end < 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Function '{}' expects at least one feature argument", getName());

        for (size_t i = 1; i < feature_end; ++i)
            if (!isNumber(arguments[i].type))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Feature argument {} of function '{}' must be numeric, got {}",
                    i, getName(), arguments[i].type->getName());

        return std::make_shared<DataTypeFloat64>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t) const override
    {
        const String model_name = getConstString(arguments[0], "model name");
        auto model = ModelRegistry::instance().getModel(model_name);

        const size_t feature_end = featureEnd(arguments);
        const String params = feature_end < arguments.size() ? getConstString(arguments.back(), "params") : String{};

        /// Feature names/order the model expects. Each argument column is inserted
        /// under the corresponding name so IModel::predict can resolve it by name.
        const auto & feature_names = model->getFeatureNames();
        const size_t n_features = feature_names.size();
        const size_t num_feature_args = feature_end - 1;

        if (num_feature_args != n_features)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Function '{}': model '{}' expects {} features but {} were supplied",
                getName(), model_name, n_features, num_feature_args);

        Block block;
        for (size_t c = 0; c < n_features; ++c)
        {
            const auto & arg = arguments[c + 1];
            block.insert({arg.column->convertToFullColumnIfConst(), arg.type, feature_names[c]});
        }

        return model->predict(block, params);
    }

private:
    /// Index one past the last feature argument. The trailing `params` (a String)
    /// is excluded; everything from index 1 up to this bound is a feature.
    /// Example:
    ///     predict(model, f1, f2, f3) -> returns 4
    ///     predict(model, f1, f2, f3, "{json: true}") -> returns 4
    static size_t featureEnd(const ColumnsWithTypeAndName & arguments)
    {
        const bool has_params = arguments.size() >= 3 && isString(arguments.back().type);
        return has_params ? arguments.size() - 1 : arguments.size();
    }

    String getConstString(const ColumnWithTypeAndName & arg, const char * what) const
    {
        const auto * col = checkAndGetColumnConst<ColumnString>(arg.column.get());
        if (!col)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                "Argument '{}' of function '{}' must be a constant String", what, getName());
        return col->getValue<String>();
    }
};

REGISTER_FUNCTION(Predict)
{
    factory.registerFunction<FunctionPredict>(FunctionDocumentation{
        .description = "Runs a trained model's prediction for each row: predict(model_name, feature1, feature2, ...[, params]).",
        .returned_value = {"The model prediction as Float64, one per row"},
        .category = FunctionDocumentation::Category::MachineLearning});
}

}
