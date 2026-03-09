#!/usr/bin/env bash

# NOTE: this sh wrapper is required because of shell_config

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

# Base case for auto case
$CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS test"
$CLICKHOUSE_CLIENT -q "CREATE TABLE test (id Int, ID Int, name String, NaMe String)"
$CLICKHOUSE_CLIENT -q "SET input_format_with_names_case_insensitive_column_matching='auto';
                       INSERT INTO test FROM INFILE '$CURDIR/data_parquet/parquet_with_names.parquet' FORMAT Parquet;"
$CLICKHOUSE_CLIENT -q "SELECT * FROM test"
$CLICKHOUSE_CLIENT -q "DROP TABLE test"

# Test ambiguity for automatic column name matching
$CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS test"
$CLICKHOUSE_CLIENT -q "CREATE TABLE test (id Int, iD Int, name String, NAME String)"
$CLICKHOUSE_CLIENT -q "SET input_format_with_names_case_insensitive_column_matching='auto';
                       INSERT INTO test FROM INFILE '$CURDIR/data_parquet/parquet_with_names.parquet' FORMAT Parquet; -- { clientError 117 }"
$CLICKHOUSE_CLIENT -q "SELECT * FROM test"
$CLICKHOUSE_CLIENT -q "DROP TABLE test"

# # Base case for match case
# $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS test"
# $CLICKHOUSE_CLIENT -q "CREATE TABLE test (id Int, ID Int, name String, NaMe String)"
# $CLICKHOUSE_CLIENT -q "SET input_format_with_names_case_insensitive_column_matching='match_case';
#                        INSERT INTO test FROM INFILE '$CURDIR/data_binary/row_binary_with_names.bin' FORMAT RowBinaryWithNames;"
# $CLICKHOUSE_CLIENT -q "SELECT * FROM test"
# $CLICKHOUSE_CLIENT -q "DROP TABLE test"

# # Base case for ignore case
# $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS test"
# $CLICKHOUSE_CLIENT -q "CREATE TABLE test (id Int, NAME String)"
# $CLICKHOUSE_CLIENT -q "SET input_format_with_names_case_insensitive_column_matching='ignore_case';
#                        INSERT INTO test FROM INFILE '$CURDIR/data_binary/row_binary_with_names_no_duplicates.bin' FORMAT RowBinaryWithNames;"
# $CLICKHOUSE_CLIENT -q "SELECT * FROM test"
# $CLICKHOUSE_CLIENT -q "DROP TABLE test"

# # Test ambiguity for ignore case column name matching
# $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS test"
# $CLICKHOUSE_CLIENT -q "CREATE TABLE test (id Int, ID Int, name String, NAME String)"
# $CLICKHOUSE_CLIENT -q "SET input_format_with_names_case_insensitive_column_matching='ignore_case';
#                        INSERT INTO test FROM INFILE '$CURDIR/data_binary/row_binary_with_names.bin' FORMAT RowBinaryWithNames; -- { clientError 117 }"
# $CLICKHOUSE_CLIENT -q "SELECT * FROM test"
# $CLICKHOUSE_CLIENT -q "DROP TABLE test"