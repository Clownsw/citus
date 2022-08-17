CREATE TYPE citus.clusterClock AS (logical bigint, counter int);
ALTER TYPE citus.clusterClock SET SCHEMA pg_catalog;

CREATE OR REPLACE FUNCTION pg_catalog.citus_get_cluster_clock()
    RETURNS clusterClock
    LANGUAGE C STABLE PARALLEL SAFE STRICT
    AS 'MODULE_PATHNAME',$$citus_get_cluster_clock$$;
COMMENT ON FUNCTION pg_catalog.citus_get_cluster_clock()
    IS 'returns monotonically increasing timestamp with logical clock value as close to epoch value (in milli seconds) as possible, and a counter for ticks(maximum of 4 million) within the logical clock';
