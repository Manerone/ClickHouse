---
slug: /sql-reference/statements/create/dictionary/layouts/xgboost
title: 'XGBoost dictionaries'
sidebar_label: 'XGBoost'
sidebar_position: 14
description: 'Configure XGBOOST dictionaries to train a gradient-boosted model and predict a numeric target.'
doc_type: 'reference'
---

The `xgboost` (`XGBOOST`) dictionary trains an [XGBoost](https://xgboost.readthedocs.io/) gradient-boosted model once, at load time, from a source table of training rows, then predicts a numeric target for any feature vector you pass in. The feature columns are the dictionary key and the single attribute is the target the model learns.

It is suited to tabular regression and classification where the features are numeric — for example forecasting a value from several measurements, or scoring rows against a learned target.

:::note
The XGBoost integration is experimental. Enable it with the `allow_experimental_xgboost` setting before creating an `XGBOOST` dictionary or calling `predictXGBoost`:

```sql
SET allow_experimental_xgboost = 1;
```
:::

You query the dictionary in one of two ways:

- [`predictXGBoost`](/sql-reference/functions/machine-learning-functions#predictxgboost) takes the features as individual arguments and returns the prediction.
- A plain [`dictGet`](/sql-reference/functions/ext-dict-functions#dictget) for the target attribute predicts too, taking the feature vector as the key (see [Notes](#notes)).

## Quickstart {#quickstart}

Here we train a regressor on the linear target `y = 2*x1 + 3*x2`.

**1. Create a source table** of training rows — the feature columns followed by the target:

```sql
CREATE TABLE training_data (x1 Float64, x2 Float64, y Float64)
ENGINE = MergeTree ORDER BY tuple();
```

**2. Insert training data:**

```sql
INSERT INTO training_data
SELECT number AS x1, number * 2 AS x2, 2 * x1 + 3 * x2 AS y
FROM numbers(100);
```

**3. Create the dictionary** with the `XGBOOST` layout — the feature columns are the key and `y` is the target attribute:

```sql
CREATE DICTIONARY model (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training_data'))
LAYOUT(XGBOOST(
    target 'y'
    objective 'reg:squarederror'
    num_iterations 100
    max_depth 6
))
LIFETIME(0);
```

`PRIMARY KEY (x1, x2)` makes `x1` and `x2` the features — but for an `XGBOOST` dictionary this "key" is the feature vector you pass in to predict, not a stored value you look up (see [Dictionary structure](#dictionary-structure)). The `LAYOUT` configures the model: `target 'y'` names the target attribute the model learns, and everything else is an XGBoost hyperparameter (see [Layout parameters](#layout-parameters)).

**4. Predict** — `predictXGBoost` takes the features positionally and returns the prediction:

```sql
SELECT predictXGBoost('model', 1.0, 2.0) AS prediction;
```

```response
   ┌─prediction─┐
1. │  7.9968586 │
   └────────────┘
```

The ground truth is `2*1 + 3*2 = 8`, so the model's prediction is close.

The same result via `dictGet`, passing the feature vector as the key:

```sql
SELECT dictGet('model', 'y', (1.0, 2.0)) AS prediction;
```

## How it works {#how-it-works}

**Training (at load time).** Each source row is a `(features..., target)` observation. When the dictionary loads, all rows are streamed into XGBoost and the model is trained once. Feature and target values are read as floats, so all key and attribute columns must be numeric.

**Predicting (at query time).** To predict, the model takes the feature vector — in the same order as the key columns were declared — and runs it through the trained booster, returning a `Float64`.

**Updating the model.** Because the model is a dictionary backed by a table, retrain by updating the table and reloading:

```sql
INSERT INTO training_data VALUES (5, 10, 40);
SYSTEM RELOAD DICTIONARY model;
```

## Dictionary structure {#dictionary-structure}

An `XGBOOST` dictionary has a fixed shape:

- The `PRIMARY KEY` is one or more numeric columns — the features. At query time this "key" is the feature vector you pass in to predict, not a stored lookup key. The feature order is the key-column declaration order, and `predictXGBoost` binds its positional arguments to that order.
- Alongside them, declare **exactly one numeric attribute**: the target the model learns. The `target` layout parameter names it, so which column is the target is stated explicitly even though it is the only attribute.

All key and attribute columns must be a native numeric type (integers and floats); a non-numeric column is rejected when the dictionary loads, not when you create it.

## Layout parameters {#layout-parameters}

Only the parameters listed below are accepted; any other name fails the load, so typos are caught when the model trains rather than being silently ignored. `target` and `num_iterations` are handled by ClickHouse (see their descriptions); every other parameter is forwarded to the XGBoost booster unchanged, as a string, and takes XGBoost's own default and value range — see the [XGBoost parameter reference](https://xgboost.readthedocs.io/en/stable/parameter.html).

| Parameter | Description |
| --- | --- |
| `target` | **Required.** Names the attribute that holds the label. This is a dictionary-structure parameter, not an XGBoost parameter: it must name the single attribute and is never sent to XGBoost. |
| `num_iterations` | Number of boosting rounds (how many trees to train). A positive integer, used as the training loop count rather than forwarded to the booster. Default `100`. |
| `booster` | Booster type: `gbtree`, `gblinear`, or `dart`. |
| `objective` | Learning objective, e.g. `reg:squarederror`, `binary:logistic`, `multi:softmax`. |
| `eval_metric` | Evaluation metric(s) used during training. |
| `seed` | Random number seed. |
| `verbosity` | Logging verbosity: `0` (silent) to `3` (debug). |
| `nthread` | Number of parallel threads used for training. |
| `eta` / `learning_rate` | Step-size shrinkage applied after each boosting round (aliases). |
| `gamma` | Minimum loss reduction required to make a further split on a leaf. |
| `max_depth` | Maximum depth of a tree. |
| `min_child_weight` | Minimum sum of instance weight (hessian) needed in a child. |
| `max_delta_step` | Maximum delta step allowed for each leaf's output. |
| `subsample` | Fraction of the training rows sampled for each boosting round. |
| `sampling_method` | Row sampling method: `uniform` or `gradient_based`. |
| `colsample_bytree` / `colsample_bylevel` / `colsample_bynode` | Fraction of columns (features) sampled per tree / per level / per split. |
| `lambda` / `reg_lambda` | L2 regularization term on weights (aliases). |
| `alpha` / `reg_alpha` | L1 regularization term on weights (aliases). |
| `tree_method` | Tree construction algorithm: `auto`, `exact`, `approx`, or `hist`. |
| `scale_pos_weight` | Balances positive and negative weights, useful for imbalanced classes. |
| `grow_policy` | How new nodes are added to the tree: `depthwise` or `lossguide`. |
| `max_leaves` | Maximum number of leaf nodes (used with `grow_policy` `lossguide`). |
| `max_bin` | Maximum number of discrete bins used to bucket continuous features (used with `tree_method` `hist`). |
| `num_parallel_tree` | Number of trees grown per boosting round (a value `> 1` trains a boosted random forest). |

You can define the dictionary with `CREATE DICTIONARY` DDL (as in the quickstart above).

```sql
CREATE DICTIONARY model (x1 Float64, x2 Float64, y Float64)
PRIMARY KEY (x1, x2)
SOURCE(CLICKHOUSE(TABLE 'training_data'))
LAYOUT(XGBOOST(
    target 'y'
    objective 'reg:squarederror'
    num_iterations 100
    max_depth 6
    eta 0.3
))
LIFETIME(3600);
```

## Prediction parameters {#prediction-parameters}

`predictXGBoost` accepts an optional trailing JSON string of XGBoost prediction parameters, after the features:

```sql
SELECT predictXGBoost('model', 1.0, 2.0, '{"type": 0, "iteration_end": 0}');
```

These mirror the prediction parameters of XGBoost's `XGBoosterPredictFromDMatrix`. Only the keys below are accepted; any other key, or a value that is not a JSON object, fails the query.

| Parameter | Description | Default |
| --- | --- | --- |
| `type` | Prediction type: `0` value, `1` margin, `2` SHAP contributions, `3` approximate contributions, `4` feature interactions, `5` approximate interactions, `6` leaf index. | `0` |
| `iteration_begin` | First boosting iteration (tree) to include in the prediction. | `0` |
| `iteration_end` | Last boosting iteration to include; `0` uses all trees. | `0` |
| `strict_shape` | Apply stricter output-shape rules. | `false` |
| `ntree_limit` | Deprecated; limits the number of trees used. Prefer `iteration_begin` / `iteration_end`. | — |

:::note
`predictXGBoost` returns exactly one `Float64` per input row, so only prediction types that produce a single value per row are meaningful. Types that emit several values per row - such as SHAP contributions (`2`, `3`) or feature interactions (`4`, `5`) - are not supported by this function.
:::

## Notes {#notes}

- **Computational dictionary semantics.** This is a *computational* dictionary: `dictGet(dict, '<target>', (feature_1, ...))` predicts for the given feature vector (the key is the input to predict, not a stored key), and `dictHas` always returns `1`. The dictionary cannot be read back as a table with `SELECT * FROM dict`.
- **Numeric columns only.** Every feature (key) column and the target attribute must be a native numeric type. Values are read as floats during training and prediction.
- **Feature order matters.** `predictXGBoost` binds its positional feature arguments to the key columns in declaration order, and the number of feature arguments must match the number of key columns.
- **Non-determinism.** `predictXGBoost` is non-deterministic because a dictionary can be reloaded (retrained) under the same name.
