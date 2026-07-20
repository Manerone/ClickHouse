-- Tags: no-fasttest
-- no-fasttest: needs the XGBoost contrib, which is not built in the fast test.

-- The XGBoost integration is experimental and must be enabled explicitly.
SET allow_experimental_xgboost = 1;

-- Note on why the prediction is never wrapped in count(): count() over a non-nullable column is optimized
-- to count the rows without evaluating its argument, so count(predictXGBoost(...)) would skip the
-- prediction entirely - and with it the dictionary load, feature-count check, and parameter validation
-- that happen when the function executes. Every query below therefore consumes the predicted value
-- (sum(isFinite(...)), a scalar SELECT, or an equality) so the prediction actually runs.

-- ============================================================================
-- Setup
-- ============================================================================

DROP DICTIONARY IF EXISTS model_04509_xgb;
DROP DICTIONARY IF EXISTS model_04509_bad;
DROP DICTIONARY IF EXISTS model_04509_not_xgb;
DROP DICTIONARY IF EXISTS model_04509_eager;

DROP TABLE IF EXISTS training_04509;
DROP TABLE IF EXISTS inference_04509;
DROP TABLE IF EXISTS training_04509_non_numeric;
DROP TABLE IF EXISTS not_xgb_04509_src;

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

-- Feature-only table for inference: exactly the feature columns, no target.
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
-- Positive: an XGBOOST dictionary with explicit hyperparameters
-- ============================================================================

-- The feature columns are the key and the single attribute is the target.
CREATE DICTIONARY model_04509_xgb (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'y' max_depth 4 eta 0.3 objective 'reg:squarederror' num_iterations 10))
LIFETIME(0);

-- `predictXGBoost` is a row-wise function returning one Float64 per input row. Exact XGBoost outputs
-- are platform-dependent, so assert deterministic, structural properties: every row predicts a finite
-- value, and the result type is Float64.
SELECT sum(isFinite(predictXGBoost('model_04509_xgb', x1, x2))) FROM inference_04509;
SELECT any(toTypeName(predictXGBoost('model_04509_xgb', x1, x2))) FROM inference_04509;

-- `dictGet` reaches the same model and must agree with `predictXGBoost` on every row.
SELECT sum(isFinite(dictGet('model_04509_xgb', 'y', (x1, x2)))) FROM inference_04509;
SELECT sum(predictXGBoost('model_04509_xgb', x1, x2) = dictGet('model_04509_xgb', 'y', (x1, x2))) FROM inference_04509;

-- ============================================================================
-- Positive: default hyperparameters (empty layout apart from the required target)
-- ============================================================================

DROP DICTIONARY model_04509_xgb;
CREATE DICTIONARY model_04509_xgb (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'y'))
LIFETIME(0);

SELECT sum(isFinite(predictXGBoost('model_04509_xgb', x1, x2))) FROM inference_04509;

-- ============================================================================
-- Positive: predict with prediction parameters
-- ============================================================================

-- An explicit (valid) prediction-parameters JSON as the trailing argument.
SELECT sum(isFinite(predictXGBoost('model_04509_xgb', x1, x2, '{"iteration_begin": 0, "iteration_end": 0}'))) FROM inference_04509;
SELECT sum(isFinite(predictXGBoost('model_04509_xgb', x1, x2, '{"type": 0}'))) FROM inference_04509;

-- ============================================================================
-- Negative: prediction parameters
-- ============================================================================

-- Error: unknown/forbidden prediction parameter.
SELECT predictXGBoost('model_04509_xgb', 1.0, 2.0, '{"not_a_predict_param": 1}'); -- { serverError XGBOOST_ERROR }

-- Error: prediction parameters are not valid JSON.
SELECT predictXGBoost('model_04509_xgb', 1.0, 2.0, 'not a json'); -- { serverError XGBOOST_ERROR }

-- Error: prediction parameters are valid JSON but not a JSON object.
SELECT predictXGBoost('model_04509_xgb', 1.0, 2.0, '123'); -- { serverError XGBOOST_ERROR }

-- ============================================================================
-- Negative: predictXGBoost arguments
-- ============================================================================

-- Error: a feature argument is not numeric.
SELECT predictXGBoost('model_04509_xgb', 'x', 2.0); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }

-- Error: predicting against a dictionary that does not exist.
SELECT predictXGBoost('model_04509_missing', 1.0, 2.0); -- { serverError BAD_ARGUMENTS }

-- Error: feature count mismatch (more features supplied than the model expects).
SELECT predictXGBoost('model_04509_xgb', 1.0, 2.0, 3.0); -- { serverError BAD_ARGUMENTS }

-- ============================================================================
-- Negative: predictXGBoost against a non-XGBoost dictionary
-- ============================================================================

CREATE TABLE not_xgb_04509_src (id UInt64, val Float64) ENGINE = Memory;
INSERT INTO not_xgb_04509_src VALUES (1, 42);

CREATE DICTIONARY model_04509_not_xgb (id UInt64, val Float64 DEFAULT 0)
PRIMARY KEY id SOURCE(CLICKHOUSE(TABLE 'not_xgb_04509_src')) LAYOUT(FLAT()) LIFETIME(0);

-- The dictionary loads fine for its own layout.
SELECT dictGet('model_04509_not_xgb', 'val', toUInt64(1));

-- But predictXGBoost rejects it because it is not an XGBOOST dictionary.
SELECT predictXGBoost('model_04509_not_xgb', 1.0); -- { serverError BAD_ARGUMENTS }

DROP DICTIONARY model_04509_not_xgb;
DROP TABLE not_xgb_04509_src;

-- ============================================================================
-- Negative: bad hyperparameters (rejected when the model trains, at first use)
-- ============================================================================

-- Error: unknown/forbidden training parameter.
CREATE DICTIONARY model_04509_bad (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'y' not_a_training_param 1)) LIFETIME(0);
SELECT predictXGBoost('model_04509_bad', 1.0, 2.0); -- { serverError XGBOOST_ERROR }
DROP DICTIONARY model_04509_bad;

-- Error: num_iterations must be a positive integer.
CREATE DICTIONARY model_04509_bad (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'y' num_iterations 0)) LIFETIME(0);
SELECT predictXGBoost('model_04509_bad', 1.0, 2.0); -- { serverError XGBOOST_ERROR }
DROP DICTIONARY model_04509_bad;

-- Error: 'target' is required.
CREATE DICTIONARY model_04509_bad (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST()) LIFETIME(0);
SELECT predictXGBoost('model_04509_bad', 1.0, 2.0); -- { serverError BAD_ARGUMENTS }
DROP DICTIONARY model_04509_bad;

-- Error: 'target' names a column that is not the single attribute.
CREATE DICTIONARY model_04509_bad (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'x1')) LIFETIME(0);
SELECT predictXGBoost('model_04509_bad', 1.0, 2.0); -- { serverError BAD_ARGUMENTS }
DROP DICTIONARY model_04509_bad;

-- ============================================================================
-- Negative: non-numeric structure (rejected at first use)
-- ============================================================================

CREATE TABLE training_04509_non_numeric
(
    x1 String,
    x2 String,
    y String
)
ENGINE = MergeTree
ORDER BY tuple();

INSERT INTO training_04509_non_numeric VALUES ('a0', 'b0', 'c0'), ('a1', 'b1', 'c1');

CREATE DICTIONARY model_04509_bad (x1 String, x2 String, y String DEFAULT '')
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509_non_numeric'))
LAYOUT(XGBOOST(target 'y')) LIFETIME(0);
-- Numeric literal features so the function's own argument-type check passes; the dictionary load then
-- rejects the String key/attribute structure with BAD_ARGUMENTS.
SELECT predictXGBoost('model_04509_bad', 1.0, 2.0); -- { serverError BAD_ARGUMENTS }
DROP DICTIONARY model_04509_bad;

-- ============================================================================
-- Train on demand with SYSTEM RELOAD DICTIONARY
-- ============================================================================

-- By default the model is trained lazily, on first use. SYSTEM RELOAD DICTIONARY trains it synchronously
-- on demand instead: after the reload the dictionary is LOADED before any predictXGBoost / dictGet uses
-- it, and a bad configuration is rejected at the reload (which rethrows the training/config error) rather
-- than on first use.

CREATE DICTIONARY model_04509_eager (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'y' objective 'reg:squarederror' num_iterations 10))
LIFETIME(0);

-- Trained by the reload: the dictionary is LOADED before any predictXGBoost / dictGet uses it.
SYSTEM RELOAD DICTIONARY model_04509_eager;
SELECT status FROM system.dictionaries WHERE database = currentDatabase() AND name = 'model_04509_eager';
SELECT isFinite(predictXGBoost('model_04509_eager', 1.0, 2.0));
DROP DICTIONARY model_04509_eager;

-- A bad configuration is rejected at SYSTEM RELOAD DICTIONARY, which forces training synchronously and
-- rethrows the error.

-- Error: num_iterations must be a positive integer.
CREATE DICTIONARY model_04509_eager (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST(target 'y' num_iterations 0)) LIFETIME(0);
SYSTEM RELOAD DICTIONARY model_04509_eager; -- { serverError XGBOOST_ERROR }
DROP DICTIONARY model_04509_eager;

-- Error: 'target' is required.
CREATE DICTIONARY model_04509_eager (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2) SOURCE(CLICKHOUSE(TABLE 'training_04509'))
LAYOUT(XGBOOST()) LIFETIME(0);
SYSTEM RELOAD DICTIONARY model_04509_eager; -- { serverError BAD_ARGUMENTS }
DROP DICTIONARY model_04509_eager;

-- ============================================================================
-- Cleanup
-- ============================================================================

DROP DICTIONARY model_04509_xgb;
DROP TABLE training_04509_non_numeric;
DROP TABLE training_04509;
DROP TABLE inference_04509;
