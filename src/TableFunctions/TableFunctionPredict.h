#pragma once

#include <optional>

#include <TableFunctions/ITableFunction.h>
#include <DataTypes/DataTypesNumber.h>
#include <Storages/StorageMemory.h>
#include <Storages/MemorySettings.h>

namespace DB
{

class TableFunctionPredict : public ITableFunction
{
public:
    static constexpr auto name = "predict";
    std::string getName() const override { return name; }

    void parseArguments(const ASTPtr & ast_function, ContextPtr context) override;

    /// The model and table names are identifiers, not expressions, so the analyzer must not
    /// try to resolve them as columns.
    VectorWithMemoryTracking<size_t> skipAnalysisForArguments(const QueryTreeNodePtr & query_node_table_function, ContextPtr context) const override;

    ColumnsDescription getActualTableStructure(ContextPtr /* context */, bool /* is_insert_query */) const override
    {
        return ColumnsDescription{{"prediction", std::make_shared<DataTypeFloat64>()}};
    }

private:
    StoragePtr executeImpl(
        const ASTPtr & ast_function,
        ContextPtr context,
        const std::string & table_name,
        ColumnsDescription cached_columns,
        bool is_insert_query) const override;

    /// The result is served from a StorageValues, which is not registered as a table engine,
    /// so there is no source-access object to check (same as TableFunctionValues).
    const char * getStorageEngineName() const override { return ""; }

    String model_name;
    String table_name;
};

}
