#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
# define CITO_NATIVE_API __declspec(dllexport)
#else
# define CITO_NATIVE_API __attribute__((visibility("default")))
#endif
typedef struct cito_native_runtime cito_native_runtime_t;
typedef struct cito_native_options {
    uint64_t coordinator;
    uint64_t incarnation;
    uint64_t destination;
    uint16_t control_port;
    uint16_t quic_port;
    uint16_t announcement_port;
    const char* announcement_ipv4;
    const char* server_cert;
    const char* server_key;
    const char* client_ca;
    const char* trusted_server_ca;
    const char* client_cert;
    const char* client_key;
} cito_native_options_t;

/* Create and install the native driver in this process. Returns null on failure.
   The caller must run poll() on exactly one owner thread and destroy after
   all public Context objects have been destroyed. */
CITO_NATIVE_API cito_native_runtime_t* cito_native_open(
    const cito_native_options_t* options);
CITO_NATIVE_API size_t cito_native_poll(cito_native_runtime_t* runtime);
CITO_NATIVE_API void cito_native_close(cito_native_runtime_t* runtime);
#ifdef __cplusplus
}
#endif
