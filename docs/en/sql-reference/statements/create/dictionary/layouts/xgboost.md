---
slug: /sql-reference/statements/create/dictionary/layouts/xgboost
title: 'XGBoost dictionaries'
sidebar_label: 'XGBoost'
sidebar_position: 14
description: 'Configure XGBOOST dictionaries to train a gradient-boosted model and predict a numeric target.'
doc_type: 'reference'
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

The `xgboost` (`XGBOOST`) dictionary trains an [XGBoost](https://xgboost.readthedocs.io/) gradient-boosted model once, at load time, from a source table of training rows, then predicts a numeric target for any feature vector you pass in. The feature columns are the dictionary key and the single attribute is the target the model learns.

It is suited to tabular regression and classification where the features are numeric — for example forecasting a value from several measurements, or scoring rows against a learned target.

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
LAYOUT(XGBOOST(target 'y' objective 'reg:squarederror' num_iterations 100 max_depth 6))
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

Every layout parameter other than `target` is passed straight through to XGBoost as a training hyperparameter and validated when the model trains.

| Parameter | Description | Example | Default |
| --- | --- | --- | --- |
| `target` | Name of the attribute that holds the target. Must name the single attribute. | `'y'` | *Required* |
| `num_iterations` | Number of boosting rounds. Must be a positive integer. | `100` | `100` |
| `objective` | XGBoost learning objective. | `'reg:squarederror'` | XGBoost default |
| `max_depth` | Maximum tree depth. | `6` | XGBoost default |
| `eta` | Learning rate (also `learning_rate`). | `0.3` | XGBoost default |
| ... | Any other supported XGBoost training parameter (`subsample`, `lambda`, `tree_method`, `seed`, ...). | | XGBoost default |

An unknown or unsupported hyperparameter fails the load. You can define the dictionary with `CREATE DICTIONARY` DDL (as in the quickstart above) or in an XML configuration file; see [Dictionary layouts](/sql-reference/statements/create/dictionary/layouts) for where that file goes.

<Tabs>
<TabItem value="ddl" label="DDL" default>

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

</TabItem>
<TabItem value="xml" label="Configuration file">

```xml
<dictionary>
    <name>model</name>
    <structure>
        <key>
            <attribute>
                <name>x1</name>
                <type>Float64</type>
            </attribute>
            <attribute>
                <name>x2</name>
                <type>Float64</type>
            </attribute>
        </key>
        <attribute>
            <name>y</name>
            <type>Float64</type>
            <null_value>0</null_value>
        </attribute>
    </structure>
    <source>
        <clickhouse>
            <table>training_data</table>
        </clickhouse>
    </source>
    <layout>
        <xgboost>
            <target>y</target>
            <objective>reg:squarederror</objective>
            <num_iterations>100</num_iterations>
            <max_depth>6</max_depth>
            <eta>0.3</eta>
        </xgboost>
    </layout>
    <lifetime>3600</lifetime>
</dictionary>
```

</TabItem>
</Tabs>

## Prediction parameters {#prediction-parameters}

`predictXGBoost` accepts an optional trailing JSON string of XGBoost prediction parameters, after the features:

```sql
SELECT predictXGBoost('model', 1.0, 2.0, '{"type": 0, "iteration_end": 0}');
```

Supported keys include `type`, `iteration_begin`, `iteration_end`, and `strict_shape`. An unknown key, or a value that is not a JSON object, fails the query.

## Notes {#notes}

- **Computational dictionary semantics.** This is a *computational* dictionary: `dictGet(dict, '<target>', (feature_1, ...))` predicts for the given feature vector (the key is the input to predict, not a stored key), and `dictHas` always returns `1`. The dictionary cannot be read back as a table with `SELECT * FROM dict`.
- **Numeric columns only.** Every feature (key) column and the target attribute must be a native numeric type. Values are read as floats during training and prediction.
- **Feature order matters.** `predictXGBoost` binds its positional feature arguments to the key columns in declaration order, and the number of feature arguments must match the number of key columns.
- **Non-determinism.** `predictXGBoost` is non-deterministic because a dictionary can be reloaded (retrained) under the same name.
