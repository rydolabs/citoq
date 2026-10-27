#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
# define CITOQ_NATIVE_API __declspec(dllexport)
#else
# define CITOQ_NATIVE_API __attribute__((visibility("default")))
#endif
typedef struct citoq_native_runtime citoq_native_runtime_t;
typedef struct citoq_native_options {
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
} citoq_native_options_t;

/* Create and install the native driver in this process. Returns null on failure.
   The caller must run poll() on exactly one owner thread and destroy after
   all public Context objects have been destroyed. */
CITOQ_NATIVE_API citoq_native_runtime_t* citoq_native_open(
    const citoq_native_options_t* options);
CITOQ_NATIVE_API size_t citoq_native_poll(citoq_native_runtime_t* runtime);
/* Single-owner event-driven poll, max_wait_ms bounds blocking and can be 0.
   Context callbacks execute synchronously on the calling owner thread. */
CITOQ_NATIVE_API size_t citoq_native_poll_wait(
    citoq_native_runtime_t* runtime, uint32_t max_wait_ms);
CITOQ_NATIVE_API void citoq_native_close(citoq_native_runtime_t* runtime);
#ifdef __cplusplus
}
#endif
