#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterLibSqlite3Shims(ShimRegistry& registry) {
    static constexpr std::array kFunctions{
        "_sqlite3_bind_blob",
        "_sqlite3_bind_double",
        "_sqlite3_bind_int",
        "_sqlite3_bind_text",
        "_sqlite3_close",
        "_sqlite3_column_blob",
        "_sqlite3_column_bytes",
        "_sqlite3_column_double",
        "_sqlite3_column_int",
        "_sqlite3_column_text",
        "_sqlite3_errmsg",
        "_sqlite3_exec",
        "_sqlite3_finalize",
        "_sqlite3_open",
        "_sqlite3_open_v2",
        "_sqlite3_prepare_v2",
        "_sqlite3_reset",
        "_sqlite3_step",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
