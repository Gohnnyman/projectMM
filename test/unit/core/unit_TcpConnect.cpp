// @module platform

// Pins platform::TcpConnection::connect — the outbound TCP client primitive MqttModule uses to reach
// a broker. It resolves a host (name or dotted-quad via getaddrinfo) and connects non-blocking,
// bounded by a timeout, leaving the socket non-blocking on success. Here we drive it over loopback
// against a real TcpServer (deterministic on desktop): connect to a listening port succeeds and the
// server accepts the connection; connect to a dead port / bad host fails cleanly (no hang, no crash).

#include "doctest.h"
#include "platform/platform.h"

#include <cstdint>

using namespace mm;

// Bind a TcpServer on an ephemeral-ish port, connect to it, and confirm the server accepts. Ports in
// the test range are unlikely to collide; if open() fails (port busy) we skip rather than flake.
TEST_CASE("platform::TcpConnection::connect reaches a listening loopback server") {
    platform::TcpServer server;
    uint16_t port = 0;
    for (uint16_t p = 34567; p < 34600; p++) {   // find a free port in a small window
        if (server.open(p)) { port = p; break; }
    }
    REQUIRE(port != 0);   // a free port was found

    platform::TcpConnection client;
    // "127.0.0.1" is a dotted-quad → getaddrinfo resolves it without a DNS query.
    const bool connected = client.connect("127.0.0.1", port, 2000);
    CHECK(connected);
    CHECK(client.valid());

    // The server sees the pending connection (accept is non-blocking; poll briefly).
    platform::TcpConnection accepted;
    for (int i = 0; i < 100 && !accepted.valid(); i++) accepted = server.accept();
    CHECK(accepted.valid());

    client.close();
    server.close();
}

// A connect to a port with nothing listening fails cleanly and quickly — no hang past the timeout,
// no crash. (Loopback refuses immediately; the timeout is the upper bound.)
TEST_CASE("platform::TcpConnection::connect fails cleanly on a refused port") {
    platform::TcpConnection client;
    // 1 is a privileged port nothing in the test binds — connection refused.
    const bool connected = client.connect("127.0.0.1", 1, 1000);
    CHECK_FALSE(connected);
    CHECK_FALSE(client.valid());
}

// A bad/empty host is rejected without touching the socket layer (no getaddrinfo hang).
TEST_CASE("platform::TcpConnection::connect rejects an empty host") {
    platform::TcpConnection client;
    CHECK_FALSE(client.connect("", 1883, 1000));
    CHECK_FALSE(client.connect(nullptr, 1883, 1000));
    CHECK_FALSE(client.valid());
}

// connectStart/connectPoll — the NON-BLOCKING outbound connect MqttModule uses so it never stalls
// Scheduler::tick (reviewer #2). connectStart returns immediately; connectPoll reports
// Pending/Connected/Failed without blocking. Over loopback the connect completes within a few polls.
TEST_CASE("platform::TcpConnection::connectStart/connectPoll is non-blocking") {
    using CR = platform::TcpConnection::ConnectResult;
    platform::TcpServer server;
    uint16_t port = 0;
    for (uint16_t p = 34700; p < 34740; p++) { if (server.open(p)) { port = p; break; } }
    REQUIRE(port != 0);

    platform::TcpConnection client;
    REQUIRE(client.connectStart("127.0.0.1", port));   // returns immediately, connect in flight
    // Poll until connected, accepting on the server side and yielding a little between polls so the
    // loopback TCP handshake can complete (connectPoll never blocks, so a tight spin would just burn
    // 200 microseconds before the kernel finishes the handshake).
    platform::TcpConnection accepted;
    CR r = CR::Pending;
    for (int i = 0; i < 200 && r == CR::Pending; i++) {
        if (!accepted.valid()) accepted = server.accept();
        r = client.connectPoll();
        if (r == CR::Pending) platform::delayMs(1);
    }
    CHECK(r == CR::Connected);
    CHECK(client.valid());

    for (int i = 0; i < 100 && !accepted.valid(); i++) { accepted = server.accept(); platform::delayMs(1); }
    CHECK(accepted.valid());

    client.close();
    server.close();
}

// connectStart on a bad/empty host fails immediately (no hang); connectPoll on an unstarted
// connection reports Failed rather than blocking.
TEST_CASE("platform::TcpConnection::connectStart rejects a bad host; poll on no-fd is Failed") {
    platform::TcpConnection client;
    CHECK_FALSE(client.connectStart("", 1883));
    CHECK_FALSE(client.connectStart(nullptr, 1883));
    CHECK(client.connectPoll() == platform::TcpConnection::ConnectResult::Failed);
}
