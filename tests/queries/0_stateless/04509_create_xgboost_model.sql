-- Tags: no-fasttest
-- no-fasttest: needs the XGBoost contrib, which is not built in the fast test.

-- ============================================================================
-- Setup
-- ============================================================================

-- The model registry is process-global (not database-scoped), using names that
-- are unique to this test
DROP MODEL IF EXISTS model_04509_xgb;
DROP MODEL IF EXISTS model_04509_missing;
DROP MODEL IF EXISTS model_04509_xgb_error;

DROP TABLE IF EXISTS training_04509;
DROP TABLE IF EXISTS inference_04509;
DROP TABLE IF EXISTS training_04509_non_numeric;

CREATE TABLE training_04509
(
    x1 Float64,
    x2 Float64,
    y Float64
)
ENGINE = MergeTree
ORDER BY tuple();

-- Small deterministic dataset: y = 2 * x1 + 3 * x2.
INSERT INTO training_04509 (x1, x2, y)
SELECT
    number AS x1,
    number * 2 AS x2,
    2 * x1 + 3 * x2 AS y
FROM numbers(100);

SELECT count() FROM training_04509;

-- Feature-only table for inference: exactly the trained feature columns, no target.
CREATE TABLE inference_04509
(
    x1 Float64,
    x2 Float64
)
ENGINE = MergeTree
ORDER BY tuple();

INSERT INTO inference_04509 (x1, x2)
SELECT number AS x1, number * 2 AS x2 FROM numbers(10);

-- ============================================================================
-- Positive: training with explicit hyper-parameters
-- ============================================================================

-- Train an XGBoost model with explicit hyper-parameters.
CREATE MODEL model_04509_xgb
ALGORITHM 'xgboost'
OPTIONS (max_depth = 4, eta = 0.3, objective = 'reg:squarederror', num_iterations = 10)
TARGET 'y'
FROM TABLE training_04509;

-- `predict` is a row-wise function returning one Float64 per input row. Exact
-- XGBoost outputs are platform-dependent, only assert deterministic, structural
-- properties here.
SELECT count(predict('model_04509_xgb', x1, x2)) FROM inference_04509;
SELECT any(toTypeName(predict('model_04509_xgb', x1, x2))) FROM inference_04509;

-- ============================================================================
-- Positive: training with default hyper-parameters
-- ============================================================================

-- OPTIONS is optional: train again with default hyper-parameters.
DROP MODEL model_04509_xgb;
CREATE MODEL model_04509_xgb
ALGORITHM 'xgboost'
TARGET 'y'
FROM TABLE training_04509;

SELECT count(predict('model_04509_xgb', x1, x2)) FROM inference_04509;

-- ============================================================================
-- Positive: predict with prediction parameters
-- ============================================================================

-- Predict with an explicit (valid) prediction-parameters JSON as the trailing
-- argument. Same deterministic, structural assertions as above.
SELECT count(predict('model_04509_xgb', x1, x2, '{"iteration_begin": 0, "iteration_end": 0}')) FROM inference_04509;
SELECT count(predict('model_04509_xgb', x1, x2, '{"type": 0}')) FROM inference_04509;
SELECT any(toTypeName(predict('model_04509_xgb', x1, x2, '{"type": 0}'))) FROM inference_04509;

-- ============================================================================
-- Negative: prediction parameters
-- ============================================================================

-- Error: unknown/forbidden prediction parameter.
SELECT count(predict('model_04509_xgb', x1, x2, '{"not_a_predict_param": 1}')) FROM inference_04509; -- { serverError XGBOOST_ERROR }

-- Error: prediction parameters are not valid JSON.
SELECT count(predict('model_04509_xgb', x1, x2, 'not a json')) FROM inference_04509; -- { serverError XGBOOST_ERROR }

-- Error: prediction parameters are valid JSON but not a JSON object.
SELECT count(predict('model_04509_xgb', x1, x2, '123')) FROM inference_04509; -- { serverError XGBOOST_ERROR }

-- Error: a feature argument is not numeric.
SELECT count(predict('model_04509_xgb', toString(x1), x2)) FROM inference_04509; -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }

-- Error: predicting against a model that does not exist.
SELECT count(predict('model_04509_missing', x1, x2)) FROM inference_04509; -- { serverError MODEL_NOT_FOUND }

-- Error: feature count mismatch (more features supplied than the model expects).
SELECT count(predict('model_04509_xgb', x1, x2, y)) FROM training_04509; -- { serverError BAD_ARGUMENTS }

-- ============================================================================
-- Negative: CREATE MODEL
-- ============================================================================

-- Error: unknown algorithm.
CREATE MODEL model_04509_missing
ALGORITHM 'not_a_real_algorithm'
TARGET 'y'
FROM TABLE training_04509; -- { serverError MODEL_NOT_FOUND }

-- Error: unknown/forbidden training parameter in OPTIONS.
CREATE MODEL model_04509_missing
ALGORITHM 'xgboost'
OPTIONS (not_a_training_param = 1)
TARGET 'y'
FROM TABLE training_04509; -- { serverError XGBOOST_ERROR }

-- Error: num_iterations must be a positive integer.
CREATE MODEL model_04509_missing
ALGORITHM 'xgboost'
OPTIONS (num_iterations = 0)
TARGET 'y'
FROM TABLE training_04509; -- { serverError XGBOOST_ERROR }

-- Error: target column does not exist in the table.
CREATE MODEL model_04509_missing
ALGORITHM 'xgboost'
TARGET 'does_not_exist'
FROM TABLE training_04509; -- { serverError THERE_IS_NO_COLUMN }

-- Error: source table does not exist.
CREATE MODEL model_04509_missing
ALGORITHM 'xgboost'
TARGET 'y'
FROM TABLE table_04509_does_not_exist; -- { serverError UNKNOWN_TABLE }

-- Error: registering a model whose name is already taken.
CREATE MODEL model_04509_xgb
ALGORITHM 'xgboost'
TARGET 'y'
FROM TABLE training_04509; -- { serverError MODEL_ALREADY_EXISTS }

-- ============================================================================
-- DROP MODEL behavior
-- ============================================================================

-- DROP MODEL removes it; IF EXISTS makes a repeated drop a no-op.
DROP MODEL model_04509_xgb;
DROP MODEL IF EXISTS model_04509_xgb;

-- Error: dropping a model that does not exist without IF EXISTS.
DROP MODEL model_04509_xgb; -- { serverError MODEL_NOT_FOUND }

-- ============================================================================
-- Negative: non-numeric training data
-- ============================================================================

-- Check error on table with non-numeric columns
CREATE TABLE training_04509_non_numeric
(
    x1 String,
    x2 String,
    y String
)
ENGINE = MergeTree
ORDER BY tuple();

INSERT INTO training_04509_non_numeric VALUES
('a0', 'b0', 'c0'),
('a1', 'b1', 'c1'),
('a2', 'b2', 'c2');

CREATE MODEL model_04509_xgb_error
ALGORITHM 'xgboost'
TARGET 'y'
FROM TABLE training_04509_non_numeric; -- { serverError XGBOOST_ERROR }

-- ============================================================================
-- Cleanup
-- ============================================================================

DROP TABLE training_04509_non_numeric;
DROP TABLE training_04509;
DROP TABLE inference_04509;
