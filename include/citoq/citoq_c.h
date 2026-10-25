#ifndef CITOQ_CITOQ_C_H
#define CITOQ_CITOQ_C_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#  if defined(CITOQ_C_BUILD)
#    define CITOQ_C_API __declspec(dllexport)
#  else
#    define CITOQ_C_API __declspec(dllimport)
#  endif
#else
#  define CITOQ_C_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define CITOQ_C_ABI_VERSION 1u
typedef struct citoq_context citoq_context_t;
typedef struct citoq_subscription citoq_subscription_t;
typedef struct citoq_handler citoq_handler_t;
typedef struct citoq_request citoq_request_t;
typedef enum citoq_status {
    CITOQ_STATUS_OK = 0,
    CITOQ_STATUS_INVALID_ARGUMENT = 1,
    CITOQ_STATUS_OUT_OF_MEMORY = 2,
    CITOQ_STATUS_INTERNAL_ERROR = 3
} citoq_status_t;

typedef enum citoq_request_status {
    CITOQ_REQUEST_OK = 0,
    CITOQ_REQUEST_CANCELLED = 1,
    CITOQ_REQUEST_UNAVAILABLE = 2,
    CITOQ_REQUEST_INVALID_ARGUMENT = 3,
    CITOQ_REQUEST_DEADLINE_EXCEEDED = 4,
    CITOQ_REQUEST_INTERNAL = 5
} citoq_request_status_t;

typedef void (*citoq_message_callback_t)(
    void* user_data, uint64_t type_id,
    const char* resource, size_t resource_size,
    const uint8_t* payload, size_t payload_size);

typedef void (*citoq_request_handler_callback_t)(
    void* user_data,
    uint64_t request_id,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    const uint8_t* payload,
    size_t payload_size);

typedef void (*citoq_request_completion_callback_t)(
    void* user_data,
    citoq_request_status_t status,
    const uint8_t* payload,
    size_t payload_size);

CITOQ_C_API uint32_t citoq_abi_version(void);
CITOQ_C_API citoq_status_t citoq_context_create(
    const char* scope, size_t scope_size, citoq_context_t** out_context);
CITOQ_C_API void citoq_context_destroy(citoq_context_t* context);
CITOQ_C_API citoq_status_t citoq_context_subscribe(
    citoq_context_t* context, uint64_t type_id,
    const char* resource, size_t resource_size,
    citoq_message_callback_t callback, void* user_data,
    citoq_subscription_t** out_subscription);
CITOQ_C_API void citoq_subscription_destroy(citoq_subscription_t* subscription);

CITOQ_C_API citoq_status_t citoq_context_handle(
    citoq_context_t* context,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    citoq_request_handler_callback_t callback,
    void* user_data,
    citoq_handler_t** out_handler);
CITOQ_C_API void citoq_handler_destroy(citoq_handler_t* handler);

CITOQ_C_API citoq_status_t citoq_context_discover_handler(
    citoq_context_t* context, uint64_t request_type_id,
    const char* resource, size_t resource_size);
CITOQ_C_API citoq_status_t citoq_context_has_handler(
    citoq_context_t* context,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    int* out_has_handler);

CITOQ_C_API citoq_status_t citoq_context_request(
    citoq_context_t* context,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    const uint8_t* payload,
    size_t payload_size,
    citoq_request_completion_callback_t callback,
    void* user_data,
    citoq_request_t** out_request);
CITOQ_C_API citoq_status_t citoq_request_cancel(
    citoq_request_t* request,
    int* out_cancelled);
CITOQ_C_API void citoq_request_destroy(citoq_request_t* request);

CITOQ_C_API citoq_status_t citoq_context_reply(
    citoq_context_t* context,
    uint64_t request_id,
    citoq_request_status_t status,
    const uint8_t* payload,
    size_t payload_size,
    int* out_accepted);

CITOQ_C_API citoq_status_t citoq_context_has_demand(
    citoq_context_t* context, uint64_t type_id,
    const char* resource, size_t resource_size,
    int* out_has_demand);
CITOQ_C_API citoq_status_t citoq_context_advertise(
    citoq_context_t* context, uint64_t type_id,
    const char* resource, size_t resource_size);
CITOQ_C_API citoq_status_t citoq_context_publish(
    citoq_context_t* context, uint64_t type_id,
    const char* resource, size_t resource_size,
    const uint8_t* payload, size_t payload_size,
    size_t* out_delivered);
CITOQ_C_API citoq_status_t citoq_context_run(citoq_context_t* context);
CITOQ_C_API void citoq_context_stop(citoq_context_t* context);
CITOQ_C_API const char* citoq_status_string(citoq_status_t status);
#ifdef __cplusplus
}
#endif
#endif
