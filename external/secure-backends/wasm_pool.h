#ifndef SECURE_WASM_POOL_H
#define SECURE_WASM_POOL_H
#include "types.h"
#include <wasmtime.h>

/* Separate engine-owned pools for connection and business runtimes. With one
 * active job per connection, this bounds both live connections and simultaneous
 * business calls. Wasmtime resets recycled backing slots on store destruction;
 * the business runtime separately resets instances retained in its thread cache. */
enum { WASM_POOL_CAPACITY = 1024 };
typedef enum wasm_pool_configure_tag { WASM_POOL_CONFIGURE_OK, WASM_POOL_CONFIGURE_NO_SPACE } wasm_pool_configure_tag;
typedef struct wasm_pool_configure_result { wasm_pool_configure_tag tag; } wasm_pool_configure_result;
/* config is a live, caller-owned Wasmtime configuration; memory_limit is the
 * matching store's byte limit. Leaves upstream reset/guard/residency defaults. */
static inline wasm_pool_configure_result WasmPoolConfigure(wasm_config_t *config, usize memory_limit)
{
    wasmtime_pooling_allocation_config_t *pool = wasmtime_pooling_allocation_config_new();
    if (!pool) return (wasm_pool_configure_result){WASM_POOL_CONFIGURE_NO_SPACE};
    wasmtime_pooling_allocation_config_total_core_instances_set(pool, WASM_POOL_CAPACITY);
    wasmtime_pooling_allocation_config_total_memories_set(pool, WASM_POOL_CAPACITY);
    wasmtime_pooling_allocation_config_total_tables_set(pool, WASM_POOL_CAPACITY);
    wasmtime_pooling_allocation_config_max_memories_per_module_set(pool, 1);
    wasmtime_pooling_allocation_config_max_tables_per_module_set(pool, 1);
    wasmtime_pooling_allocation_config_table_elements_set(pool, 64);
    wasmtime_pooling_allocation_config_max_memory_size_set(pool, memory_limit);
    /* This API clones the settings; ownership remains here. */
    wasmtime_pooling_allocation_strategy_set(config, pool);
    wasmtime_pooling_allocation_config_delete(pool);
    return (wasm_pool_configure_result){WASM_POOL_CONFIGURE_OK};
}
#endif
