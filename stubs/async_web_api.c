#include "async_web_api.h"
#include <stdlib.h>
#include <string.h>

// Global async registry
static async_registry_t g_async_registry = {0};

void async_registry_init(void) {
    if (g_async_registry.initialized) {
        return;
    }
    
    // Initialize all operations to empty state
    for (int i = 0; i < MAX_ASYNC_OPERATIONS; i++) {
        g_async_registry.operations[i].id = -1;
        g_async_registry.operations[i].type = 0;
        g_async_registry.operations[i].state = ASYNC_STATE_PENDING;
        g_async_registry.operations[i].data = NULL;
        g_async_registry.operations[i].data_size = 0;
        g_async_registry.operations[i].error_message = NULL;
    }
    
    g_async_registry.next_id = 1;
    g_async_registry.initialized = true;
}

int32_t async_register_operation(async_op_type_t type, void *data, size_t data_size) {
    if (!g_async_registry.initialized) {
        async_registry_init();
    }
    
    // Find an available slot
    for (int i = 0; i < MAX_ASYNC_OPERATIONS; i++) {
        if (g_async_registry.operations[i].id == -1) {
            int32_t id = g_async_registry.next_id++;
            if (g_async_registry.next_id < 0) {
                g_async_registry.next_id = 1;  // Reset if overflow
            }
            
            g_async_registry.operations[i].id = id;
            g_async_registry.operations[i].type = type;
            g_async_registry.operations[i].state = ASYNC_STATE_PENDING;
            
            if (data && data_size > 0) {
                g_async_registry.operations[i].data = malloc(data_size);
                if (g_async_registry.operations[i].data) {
                    memcpy(g_async_registry.operations[i].data, data, data_size);
                    g_async_registry.operations[i].data_size = data_size;
                } else {
                    g_async_registry.operations[i].data_size = 0;
                }
            } else {
                g_async_registry.operations[i].data = NULL;
                g_async_registry.operations[i].data_size = 0;
            }
            
            g_async_registry.operations[i].error_message = NULL;
            
            return id;
        }
    }
    
    return -1; // No available slots
}

void async_update_operation(int32_t id, async_state_t state, void *result_data, size_t result_size, const char *error) {
    if (!g_async_registry.initialized) {
        return;
    }
    
    for (int i = 0; i < MAX_ASYNC_OPERATIONS; i++) {
        if (g_async_registry.operations[i].id == id) {
            g_async_registry.operations[i].state = state;
            
            // Free existing data
            if (g_async_registry.operations[i].data) {
                free(g_async_registry.operations[i].data);
            }
            
            // Set new data if provided
            if (result_data && result_size > 0) {
                // Free existing data first
                if (g_async_registry.operations[i].data) {
                    free(g_async_registry.operations[i].data);
                }
                
                g_async_registry.operations[i].data = malloc(result_size);
                if (g_async_registry.operations[i].data) {
                    memcpy(g_async_registry.operations[i].data, result_data, result_size);
                    g_async_registry.operations[i].data_size = result_size;
                } else {
                    g_async_registry.operations[i].data_size = 0;
                }
            }
            
            // Set error message if provided
            if (g_async_registry.operations[i].error_message) {
                free(g_async_registry.operations[i].error_message);
                g_async_registry.operations[i].error_message = NULL;
            }
            
            if (error) {
                size_t error_len = strlen(error) + 1;
                g_async_registry.operations[i].error_message = malloc(error_len);
                if (g_async_registry.operations[i].error_message) {
                    strcpy(g_async_registry.operations[i].error_message, error);
                }
            }
            
            break;
        }
    }
}

async_state_t async_get_operation_state(int32_t id, void **out_data, size_t *out_size, char **out_error) {
    if (!g_async_registry.initialized) {
        return ASYNC_STATE_REJECTED; // Error state
    }
    
    for (int i = 0; i < MAX_ASYNC_OPERATIONS; i++) {
        if (g_async_registry.operations[i].id == id) {
            if (out_data) {
                *out_data = g_async_registry.operations[i].data;
            }
            if (out_size) {
                *out_size = g_async_registry.operations[i].data_size;
            }
            if (out_error) {
                *out_error = g_async_registry.operations[i].error_message;
            }
            return g_async_registry.operations[i].state;
        }
    }
    
    return ASYNC_STATE_REJECTED; // Operation not found
}

void async_remove_operation(int32_t id) {
    if (!g_async_registry.initialized) {
        return;
    }
    
    for (int i = 0; i < MAX_ASYNC_OPERATIONS; i++) {
        if (g_async_registry.operations[i].id == id) {
            // Free data
            if (g_async_registry.operations[i].data) {
                free(g_async_registry.operations[i].data);
            }
            
            // Free error message
            if (g_async_registry.operations[i].error_message) {
                free(g_async_registry.operations[i].error_message);
            }
            
            // Reset the slot
            g_async_registry.operations[i].id = -1;
            g_async_registry.operations[i].type = 0;
            g_async_registry.operations[i].state = ASYNC_STATE_PENDING;
            g_async_registry.operations[i].data = NULL;
            g_async_registry.operations[i].data_size = 0;
            g_async_registry.operations[i].error_message = NULL;
            
            break;
        }
    }
}

bool async_operation_exists(int32_t id) {
    if (!g_async_registry.initialized) {
        return false;
    }
    
    for (int i = 0; i < MAX_ASYNC_OPERATIONS; i++) {
        if (g_async_registry.operations[i].id == id) {
            return true;
        }
    }
    
    return false;
}