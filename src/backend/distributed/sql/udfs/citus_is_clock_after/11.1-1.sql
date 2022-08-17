-- Note: Below UDF depends on composite type clusterClock defined in citus_get_cluster_clock()
CREATE OR REPLACE FUNCTION pg_catalog.citus_is_clock_after(clock1 clusterClock, clock2 clusterClock)
    RETURNS BOOL
    LANGUAGE C STABLE PARALLEL SAFE STRICT
    AS 'MODULE_PATHNAME',$$citus_is_clock_after$$;

COMMENT ON FUNCTION pg_catalog.citus_is_clock_after(clusterClock, clusterClock)
    IS ' accepts logical clock timestamps of two causally related events and returns true if the argument1 happened before argument2';

