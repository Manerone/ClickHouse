#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: needs the XGBoost contrib, which is not built in the fast test.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --allow_experimental_xgboost=1 --query "
DROP DICTIONARY IF EXISTS model;
DROP TABLE IF EXISTS training;

CREATE TABLE training (x1 Float64, x2 Float64, y Float64) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO training SELECT number AS x1, number * 2 AS x2, 2 * x1 + 3 * x2 AS y FROM numbers(100);

CREATE DICTIONARY model (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training'))
LAYOUT(XGBOOST(objective 'reg:squarederror' num_iterations 10))
LIFETIME(0);
"

# First use trains the model and persists it automatically at a per-dictionary path.
${CLICKHOUSE_CLIENT} --allow_experimental_xgboost=1 --query "SELECT isFinite(predictXGBoost('model', 1.0, 2.0))"

# Empty the source, then reload. Reuse is keyed to the dictionary's identity, so SYSTEM RELOAD reuses the
# persisted model instead of retraining: if it had retrained it would fail on the now-empty source with
# 'No training data was provided'. A finite prediction proves the persisted model was reused.
${CLICKHOUSE_CLIENT} --allow_experimental_xgboost=1 --query "TRUNCATE TABLE training; SYSTEM RELOAD DICTIONARY model;"
${CLICKHOUSE_CLIENT} --allow_experimental_xgboost=1 --query "SELECT isFinite(predictXGBoost('model', 1.0, 2.0))"

# The model path is chosen automatically and can no longer be supplied by the user.
${CLICKHOUSE_CLIENT} --allow_experimental_xgboost=1 --query "
CREATE DICTIONARY model_bad (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training'))
LAYOUT(XGBOOST(model_path '/some/path.ubj'))
LIFETIME(0);
" 2>&1 | grep -o -m1 "BAD_ARGUMENTS" || echo "no error"

${CLICKHOUSE_CLIENT} --query "
DROP DICTIONARY IF EXISTS model;
DROP DICTIONARY IF EXISTS model_bad;
DROP TABLE IF EXISTS training;
"
