#include <cito/native_runtime_c.h>
#include <cito/detail/linux_request_runtime_driver.hpp>

#include <arpa/inet.h>
#include <memory>
#include <string>
#include <utility>

struct cito_native_runtime {
    std::shared_ptr<cito::detail::EmbeddedHostRuntime> runtime;
    std::shared_ptr<cito::detail::LinuxRequestRuntimeDriver> driver;
};

namespace {
std::string optional_string(const char* s) { return s ? std::string(s) : std::string{}; }
}

extern "C" cito_native_runtime_t* cito_native_open(const cito_native_options_t* arg) {
    if (!arg || !arg->coordinator || !arg->incarnation) return nullptr;
    try {
        auto handle = std::make_unique<cito_native_runtime>();
        handle->runtime = cito::detail::acquire_embedded_host_runtime();
        cito::detail::LinuxRequestRuntimeOptions options;
        options.coordinator = arg->coordinator;
        options.incarnation = arg->incarnation;
        options.destination = arg->destination;
        options.control_port = arg->control_port;
        options.quic_port = arg->quic_port;
        options.lease_ms = 700;
        options.announce_interval_ms = 100;
        if (arg->announcement_ipv4 && arg->announcement_port) {
            in_addr address{};
            if (::inet_pton(AF_INET, arg->announcement_ipv4, &address) != 1)
                return nullptr;
            options.announcement_targets.push_back(
                cito::detail::UdpEndpoint{ntohl(address.s_addr),
                                           arg->announcement_port});
        }
        options.certificate_file = optional_string(arg->server_cert);
        options.private_key_file = optional_string(arg->server_key);
        options.quic.registration_name = "cito-python-native-runtime";
        options.quic.ca_certificate_file = optional_string(arg->trusted_server_ca);
        options.quic.client_certificate_file = optional_string(arg->client_cert);
        options.quic.client_private_key_file = optional_string(arg->client_key);
        if (arg->client_ca && arg->client_ca[0]) {
            options.quic.require_client_certificate = true;
            options.quic.trusted_client_ca_file = arg->client_ca;
        }
        handle->driver = cito::detail::install_linux_request_runtime(
            *handle->runtime, std::move(options));
        return handle.release();
    } catch (...) { return nullptr; }
}

extern "C" size_t cito_native_poll(cito_native_runtime_t* h) {
    if (!h || !h->driver) return 0;
    try { return h->driver->poll_once(); } catch (...) { return 0; }
}

extern "C" void cito_native_close(cito_native_runtime_t* h) {
    if (!h) return;
    try {
        if (h->runtime && h->driver)
            (void)h->runtime->clear_driver(h->driver.get());
        h->driver.reset();
    } catch (...) {}
    delete h;
}
