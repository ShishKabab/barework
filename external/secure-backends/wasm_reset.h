#ifndef SECURE_WASM_RESET_H
#define SECURE_WASM_RESET_H
#include "types.h"
/* Call only after Wasmtime has validated the module. Conservative reset profile:
 * one unshared memory32, one fixed-size funcref table without element segments,
 * one mutable i32 global (exported stack pointer), numeric function signatures,
 * active data only, no imports/start/passive segments/tags. Unknown forms use
 * fresh instances. This bounds all guest-visible mutable state except memory.size,
 * which intentionally retains its high-water mark between requests. */
bool WasmResetCompatible(const_byte_slice module);
#endif
