/// @module RtspSession
/// @also RtspDriver

/// The RTSP control conversation an H.264 stream is negotiated over. A player rejects a session on a header it cannot parse, and says so only by disconnecting. The response shape is therefore pinned here rather than by watching VLC give up.

#include "doctest.h"
#include "light/util/RtspSession.h"

#include <cstring>
#include <string>

namespace {

/// The response as text, which is what a client reads.
std::string answer(mm::rtsp::Session& s, const char* request,
                   const char* sdp = nullptr, const char* url = "rtsp://d/") {
    mm::rtsp::Request req;
    if (!mm::rtsp::parseRequest(request, std::strlen(request), &req)) return {};
    char out[1024] = {};
    const size_t n = s.respond(req, sdp, url, out, sizeof(out));
    return std::string(out, n);
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

// Every response echoes the request's CSeq: a client pairs the two by that number alone, and treats a mismatch as a lost message.
TEST_CASE("RtspSession echoes the request's CSeq") {
    mm::rtsp::Session s(42);
    const auto r = answer(s, "OPTIONS rtsp://d/ RTSP/1.0\r\nCSeq: 7\r\n\r\n");
    CHECK(has(r, "RTSP/1.0 200 OK"));
    CHECK(has(r, "CSeq: 7"));
}

// OPTIONS advertises what this server answers, which is how a client decides what to send next.
TEST_CASE("RtspSession lists the verbs it implements") {
    mm::rtsp::Session s(1);
    const auto r = answer(s, "OPTIONS rtsp://d/ RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    for (const char* v : {"OPTIONS", "DESCRIBE", "SETUP", "PLAY", "TEARDOWN"}) CHECK(has(r, v));
}

// DESCRIBE carries the SDP as a body, with a Content-Length a client reads before the body arrives.
TEST_CASE("RtspSession answers DESCRIBE with the SDP and its length") {
    mm::rtsp::Session s(1);
    const char* sdp = "v=0\r\nm=video 0 RTP/AVP 96\r\n";
    const auto r = answer(s, "DESCRIBE rtsp://d/ RTSP/1.0\r\nCSeq: 2\r\n\r\n", sdp);
    CHECK(has(r, "Content-Type: application/sdp"));
    CHECK(has(r, "Content-Length: 27"));   // the SDP above, byte for byte
    CHECK(has(r, "m=video 0 RTP/AVP 96"));
}

// SETUP agrees the transport: the client names its RTP port, the server names its own, and the session id appears in every later response.
TEST_CASE("RtspSession agrees a UDP transport and remembers the client's port") {
    mm::rtsp::Session s(99);
    CHECK(s.state() == mm::rtsp::State::Init);
    const auto r = answer(s, "SETUP rtsp://d/ RTSP/1.0\r\nCSeq: 3\r\n"
                             "Transport: RTP/AVP;unicast;client_port=6000-6001\r\n\r\n");
    CHECK(has(r, "RTSP/1.0 200 OK"));
    CHECK(has(r, "client_port=6000-6001"));
    CHECK(has(r, "Session: 99"));
    CHECK(s.state() == mm::rtsp::State::Ready);
    CHECK(s.rtpPort() == 6000);
}

// This server speaks UDP, and a client asking for interleaved TCP is told so in the code RFC 2326 assigns rather than by a silent failure.
TEST_CASE("RtspSession refuses a transport it does not speak") {
    mm::rtsp::Session s(1);
    const auto r = answer(s, "SETUP rtsp://d/ RTSP/1.0\r\nCSeq: 3\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    CHECK(has(r, "461 Unsupported Transport"));
    CHECK(s.state() == mm::rtsp::State::Init);
}

// PLAY starts a stream the transport agreed at SETUP carries, so a PLAY arriving first has nowhere to send and says so.
TEST_CASE("RtspSession refuses PLAY before SETUP") {
    mm::rtsp::Session s(1);
    const auto r = answer(s, "PLAY rtsp://d/ RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    CHECK(has(r, "455 Method Not Valid In This State"));
    CHECK(s.state() == mm::rtsp::State::Init);
}

// The whole conversation in order, which is what a player actually sends.
TEST_CASE("RtspSession walks OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN") {
    mm::rtsp::Session s(7);
    answer(s, "OPTIONS rtsp://d/ RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    answer(s, "DESCRIBE rtsp://d/ RTSP/1.0\r\nCSeq: 2\r\n\r\n", "v=0\r\n");
    answer(s, "SETUP rtsp://d/ RTSP/1.0\r\nCSeq: 3\r\n"
              "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n\r\n");
    REQUIRE(s.state() == mm::rtsp::State::Ready);

    const auto play = answer(s, "PLAY rtsp://d/ RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    CHECK(has(play, "RTSP/1.0 200 OK"));
    CHECK(has(play, "Range: npt=0.000-"));
    CHECK(s.state() == mm::rtsp::State::Playing);

    const auto down = answer(s, "TEARDOWN rtsp://d/ RTSP/1.0\r\nCSeq: 5\r\n\r\n");
    CHECK(has(down, "RTSP/1.0 200 OK"));
    CHECK(s.state() == mm::rtsp::State::Init);
    CHECK(s.rtpPort() == 0);          // the agreed transport is released with the session
}

// A header's casing is the client's choice, so CSeq is matched without regard to it.
TEST_CASE("RtspSession reads CSeq whatever its casing") {
    mm::rtsp::Session s(1);
    const auto r = answer(s, "OPTIONS rtsp://d/ RTSP/1.0\r\ncseq: 12\r\n\r\n");
    CHECK(has(r, "CSeq: 12"));
}

// A verb this server has no answer for is refused by code, which a client reports rather than hanging on.
TEST_CASE("RtspSession refuses a verb it does not implement") {
    mm::rtsp::Request req;
    CHECK_FALSE(mm::rtsp::parseRequest("PAUSE rtsp://d/ RTSP/1.0\r\nCSeq: 9\r\n\r\n",
                                       40, &req));
}

// The SDP names H.264 at the payload type the packets carry, and the geometry the encoder produces: a player reads the codec here before a single packet arrives.
TEST_CASE("RtspSession builds an SDP naming H.264 and the stream's geometry") {
    char sdp[512];
    const size_t n = mm::rtsp::buildSdp(sdp, sizeof(sdp), "192.168.1.50", 320, 240, 30, 96);
    REQUIRE(n > 0);
    const std::string s(sdp, n);
    CHECK(has(s, "m=video 0 RTP/AVP 96"));
    CHECK(has(s, "a=rtpmap:96 H264/90000"));
    CHECK(has(s, "a=framesize:96 320-240"));
    CHECK(has(s, "a=framerate:30"));
    CHECK(has(s, "192.168.1.50"));
}
