# Cito QUIC v1

Cito uses QUIC only when the runtime requires a secure remote transport. Transport choice remains internal; the application still calls publish/on.

Policy:

- same process: in-process
- same host: SHM
- remote without secure-session requirement: UDP
- secure remote BestEffort: QUIC DATAGRAM
- secure remote Reliable: QUIC stream

The first real backend validation uses MsQuic. MsQuic exposes a cross-platform C API, supports streams and the QUIC datagram extension, and uses one registration execution context shared across connections.

MsQuic callbacks are treated as external transport callbacks. They must do minimal work and hand state changes/data into Cito's event-loop ownership model; they do not become a second owner of Cito discovery/routing state.

The optional CI smoke installs MsQuic and sends an actual Cito DataPacket between two Linux network namespaces once by reliable QUIC stream and once by QUIC DATAGRAM.
