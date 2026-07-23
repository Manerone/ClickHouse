#!/usr/bin/env bash
# Tags: no-parallel
# no-parallel: creates a same-named decoy dictionary in the shared `default` database
#              and asserts on its status, so it must not run alongside other tests
#              (e.g. global `SYSTEM RELOAD/UNLOAD DICTIONARIES`).

# Regression test for `SYSTEM RELOAD/UNLOAD DICTIONARY <name> ON CLUSTER`.
# The dictionary name must be resolved against the initiator's current database
# before the query is sent to the cluster.
# To make the wrong-target case observable, a decoy dictionary with the same bare
# name `dict` is created in `default` (the worker's fallback database).

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# Decoy dictionary with the same bare name in the worker's fallback database.
${CLICKHOUSE_CLIENT} --multiquery "
DROP DICTIONARY IF EXISTS default.dict;
DROP TABLE IF EXISTS default.dict_source;
CREATE TABLE default.dict_source (id UInt64, val String) ENGINE = Memory;
INSERT INTO default.dict_source VALUES (1, 'decoy');
CREATE DICTIONARY default.dict (id UInt64, val String)
PRIMARY KEY id
SOURCE(CLICKHOUSE(TABLE 'dict_source' DB 'default'))
LAYOUT(FLAT())
LIFETIME(0);
SYSTEM RELOAD DICTIONARY default.dict;
"

${CLICKHOUSE_CLIENT} --multiquery "
DROP DICTIONARY IF EXISTS dict;
DROP TABLE IF EXISTS dict_source;

CREATE TABLE dict_source (id UInt64, val String) ENGINE = Memory;
INSERT INTO dict_source VALUES (1, 'real');

CREATE DICTIONARY dict (id UInt64, val String)
PRIMARY KEY id
SOURCE(CLICKHOUSE(TABLE 'dict_source' DB '${CLICKHOUSE_DATABASE}'))
LAYOUT(FLAT())
LIFETIME(0);

SYSTEM RELOAD DICTIONARY dict;
SELECT 'before', if(database = currentDatabase(), 'current', 'default') AS which, status
FROM system.dictionaries WHERE name = 'dict' AND database IN (currentDatabase(), 'default') ORDER BY which;

-- Disable output from ON CLUSTER DDLs
SET distributed_ddl_output_mode = 'none';

-- Must resolve 'dict' against the current database, not the worker's fallback ('default').

SYSTEM UNLOAD DICTIONARY dict ON CLUSTER test_shard_localhost;
SELECT 'after unload on cluster', if(database = currentDatabase(), 'current', 'default') AS which, status
FROM system.dictionaries WHERE name = 'dict' AND database IN (currentDatabase(), 'default') ORDER BY which;

SYSTEM RELOAD DICTIONARY dict ON CLUSTER test_shard_localhost;
SELECT 'after reload on cluster', if(database = currentDatabase(), 'current', 'default') AS which, status
FROM system.dictionaries WHERE name = 'dict' AND database IN (currentDatabase(), 'default') ORDER BY which;

DROP DICTIONARY dict;
DROP TABLE dict_source;
DROP DICTIONARY default.dict;
DROP TABLE default.dict_source;
"
