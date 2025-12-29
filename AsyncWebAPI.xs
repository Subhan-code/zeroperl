#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "async_web_api.h"

MODULE = AsyncWebAPI PACKAGE = AsyncWebAPI

void
BOOTSTRAP: AsyncWebAPI
    CODE:
        async_web_api_init();

int32_t
_async_fetch(url, method, headers, body)
    const char *url
    const char *method
    const char *headers
    const char *body
    CODE:
        RETVAL = async_fetch(url, method, headers, body);
    OUTPUT:
        RETVAL

int32_t
_async_timer(delay_ms)
    int32_t delay_ms
    CODE:
        RETVAL = async_timer(delay_ms);
    OUTPUT:
        RETVAL

bool
_async_wait_for_completion(op_id)
    int32_t op_id
    CODE:
        RETVAL = async_wait_for_completion(op_id);
    OUTPUT:
        RETVAL

int32_t
_async_check_status(op_id)
    int32_t op_id
    CODE:
        RETVAL = async_check_status(op_id, NULL, NULL, NULL);
    OUTPUT:
        RETVAL

void
_async_cleanup(op_id)
    int32_t op_id
    CODE:
        async_cleanup(op_id);

char *
_get_error_message(op_id)
    int32_t op_id
    CODE:
        char *error_msg;
        int32_t status = async_check_status(op_id, NULL, NULL, &error_msg);
        if (status == ASYNC_STATE_REJECTED && error_msg != NULL) {
            RETVAL = error_msg;
        } else {
            RETVAL = NULL;
        }
    OUTPUT:
        RETVAL

char *
_get_operation_result(op_id)
    int32_t op_id
    CODE:
        char *result_data;
        size_t result_size;
        int32_t status = async_check_status(op_id, &result_data, &result_size, NULL);
        if (status == ASYNC_STATE_RESOLVED && result_data != NULL) {
            // For now, return as string - in a real implementation, 
            // this would handle different result types appropriately
            RETVAL = result_data;
        } else {
            RETVAL = NULL;
        }
    OUTPUT:
        RETVAL