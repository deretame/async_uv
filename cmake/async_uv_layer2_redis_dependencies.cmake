include_guard(GLOBAL)

# Redis layer is migrated to Boost.Redis and no longer uses hiredis.
set(FLUX_REDIS_HAS_HIREDIS OFF)
set(FLUX_REDIS_HAS_HIREDIS_SSL OFF)
