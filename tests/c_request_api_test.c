#include <cito/cito_c.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct request_state {
    cito_context_t* provider;
    uint64_t request_id;
    int handler_calls;
    int completion_calls;
    cito_request_status_t status;
    uint8_t response[8];
    size_t response_size;
    int auto_reply;
};

static void handler_cb(
    void* user,
    uint64_t request_id,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    const uint8_t* payload,
    size_t payload_size) {
    struct request_state* s =
        (struct request_state*)user;

    ++s->handler_calls;
    s->request_id = request_id;

    assert(request_type_id == 500u ||
           request_type_id == 501u);
    assert(resource != NULL);
    assert(resource_size > 0u);
    assert(payload_size == 1u);

    if (!s->auto_reply) {
        return;
    }

    const uint8_t response[] = {
        (uint8_t)(payload[0] + 1u)
    };
    int accepted = 0;

    assert(cito_context_reply(
        s->provider,
        request_id,
        CITO_REQUEST_OK,
        response,
        sizeof response,
        &accepted) == CITO_STATUS_OK);
    assert(accepted == 1);
}

static void completion_cb(
    void* user,
    cito_request_status_t status,
    const uint8_t* payload,
    size_t payload_size) {
    struct request_state* s =
        (struct request_state*)user;

    ++s->completion_calls;
    s->status = status;
    s->response_size = payload_size;

    if (payload_size > 0u) {
        assert(payload != NULL);
        assert(payload_size <=
               sizeof s->response);
        memcpy(
            s->response,
            payload,
            payload_size);
    }
}

int main(void) {
    cito_context_t* provider = NULL;
    cito_context_t* requester = NULL;
    cito_context_t* isolated = NULL;

    assert(cito_context_create(
        "acme", 4,
        &provider) == CITO_STATUS_OK);
    assert(cito_context_create(
        "acme", 4,
        &requester) == CITO_STATUS_OK);
    assert(cito_context_create(
        "other", 5,
        &isolated) == CITO_STATUS_OK);

    int has_handler = 99;
    assert(cito_context_has_handler(
        requester,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITO_STATUS_OK);
    assert(has_handler == 0);

    struct request_state state = {
        .provider = provider,
        .auto_reply = 1
    };

    cito_handler_t* handler = NULL;
    assert(cito_context_handle(
        provider,
        500,
        "robot_1", 7,
        handler_cb,
        &state,
        &handler) ==
        CITO_STATUS_OK);

    assert(cito_context_has_handler(
        requester,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITO_STATUS_OK);
    assert(has_handler == 1);

    assert(cito_context_has_handler(
        isolated,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITO_STATUS_OK);
    assert(has_handler == 0);

    const uint8_t request_payload[] = {7};
    cito_request_t* request = NULL;

    assert(cito_context_request(
        requester,
        500,
        "robot_1", 7,
        request_payload,
        sizeof request_payload,
        completion_cb,
        &state,
        &request) ==
        CITO_STATUS_OK);

    assert(request != NULL);
    assert(state.handler_calls == 1);
    assert(state.completion_calls == 1);
    assert(state.status ==
           CITO_REQUEST_OK);
    assert(state.response_size == 1u);
    assert(state.response[0] == 8u);

    int cancelled = 99;
    assert(cito_request_cancel(
        request,
        &cancelled) ==
        CITO_STATUS_OK);
    assert(cancelled == 0);
    cito_request_destroy(request);

    /* Raw C API still reports Unavailable if the caller skips precheck. */
    state.completion_calls = 0;
    request = NULL;

    assert(cito_context_request(
        requester,
        500,
        "missing", 7,
        request_payload,
        sizeof request_payload,
        completion_cb,
        &state,
        &request) ==
        CITO_STATUS_OK);

    assert(state.completion_calls == 1);
    assert(state.status ==
           CITO_REQUEST_UNAVAILABLE);
    cito_request_destroy(request);

    /* Cancellation completes exactly once and rejects a late reply. */
    struct request_state slow = {
        .provider = provider,
        .auto_reply = 0
    };
    cito_handler_t* slow_handler = NULL;

    assert(cito_context_handle(
        provider,
        501,
        "slow", 4,
        handler_cb,
        &slow,
        &slow_handler) ==
        CITO_STATUS_OK);

    request = NULL;
    assert(cito_context_request(
        requester,
        501,
        "slow", 4,
        request_payload,
        sizeof request_payload,
        completion_cb,
        &slow,
        &request) ==
        CITO_STATUS_OK);

    assert(slow.handler_calls == 1);
    assert(slow.completion_calls == 0);
    assert(slow.request_id != 0u);

    cancelled = 0;
    assert(cito_request_cancel(
        request,
        &cancelled) ==
        CITO_STATUS_OK);
    assert(cancelled == 1);
    assert(slow.completion_calls == 1);
    assert(slow.status ==
           CITO_REQUEST_CANCELLED);

    int accepted = 99;
    const uint8_t late[] = {9};

    assert(cito_context_reply(
        provider,
        slow.request_id,
        CITO_REQUEST_OK,
        late,
        sizeof late,
        &accepted) ==
        CITO_STATUS_OK);
    assert(accepted == 0);

    cito_request_destroy(request);

    cito_handler_destroy(slow_handler);
    cito_handler_destroy(handler);

    assert(cito_context_has_handler(
        requester,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITO_STATUS_OK);
    assert(has_handler == 0);

    cito_context_destroy(isolated);
    cito_context_destroy(requester);
    cito_context_destroy(provider);

    return 0;
}
