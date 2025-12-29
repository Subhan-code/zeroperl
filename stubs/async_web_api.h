#ifndef ASYNC_WEB_API_H
#define ASYNC_WEB_API_H

#include <stdbool.h>
#include <stdint.h>

// Async operation types
typedef enum {
    ASYNC_OP_FETCH = 1,
    ASYNC_OP_TIMER = 2,
    ASYNC_OP_CUSTOM = 3
} async_op_type_t;

// Async operation state
typedef enum {
    ASYNC_STATE_PENDING = 0,
    ASYNC_STATE_RESOLVED = 1,
    ASYNC_STATE_REJECTED = 2
} async_state_t;

// Structure to track async operations
typedef struct {
    int32_t id;
    async_op_type_t type;
    async_state_t state;
    void *data;
    size_t data_size;
    char *error_message;
} async_operation_t;

// Maximum number of concurrent async operations
#ifndef MAX_ASYNC_OPERATIONS
#define MAX_ASYNC_OPERATIONS 64
#endif

// Async operation registry
typedef struct {
    async_operation_t operations[MAX_ASYNC_OPERATIONS];
    int32_t next_id;
    bool initialized;
} async_registry_t;

// Initialize the async registry
void async_registry_init(void);

// Register a new async operation and return its ID
int32_t async_register_operation(async_op_type_t type, void *data, size_t data_size);

// Update the state of an async operation
void async_update_operation(int32_t id, async_state_t state, void *result_data, size_t result_size, const char *error);

// Get the state of an async operation
async_state_t async_get_operation_state(int32_t id, void **out_data, size_t *out_size, char **out_error);

// Remove an async operation from the registry
void async_remove_operation(int32_t id);

// Check if async operation exists
bool async_operation_exists(int32_t id);

// Import functions from JavaScript for async operations
__attribute__((import_module("env"), import_name("js_async_fetch"))) 
int32_t js_async_fetch(const char *url, const char *method, const char *headers, const char *body);

__attribute__((import_module("env"), import_name("js_async_timer"))) 
int32_t js_async_timer(int32_t delay_ms);

__attribute__((import_module("env"), import_name("js_async_resolve_pending"))) 
bool js_async_resolve_pending(void);

#endif // ASYNC_WEB_API_H