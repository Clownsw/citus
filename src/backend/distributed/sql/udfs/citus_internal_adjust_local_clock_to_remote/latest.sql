CREATE OR REPLACE FUNCTION pg_catalog.citus_internal_adjust_local_clock_to_remote(logical bigint, counter int)
    RETURNS void
    LANGUAGE C STABLE PARALLEL SAFE STRICT
    AS 'MODULE_PATHNAME', $$citus_internal_adjust_local_clock_to_remote$$;
COMMENT ON FUNCTION pg_catalog.citus_internal_adjust_local_clock_to_remote(bigint, int)
    IS 'Is an internal UDF used to adjust to the transaction clock value of the remote node(s) distributed transaction id';

REVOKE ALL ON FUNCTION pg_catalog.citus_internal_adjust_local_clock_to_remote(bigint, int) FROM PUBLIC;
