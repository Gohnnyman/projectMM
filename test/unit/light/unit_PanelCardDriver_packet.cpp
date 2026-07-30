// @module PanelCardDriver

#include "doctest.h"
#include "light/ColorLight5A75Packet.h"

#include <cstring>

// The cards filter on a fixed destination MAC, so a frame sent anywhere else (broadcast, or from
// this device's own address) is discarded without any error the sender can see.
TEST_CASE("Panel frames carry the fixed MAC pair the cards filter on") {
    uint8_t packet[mm::COLORLIGHT_MAX_FRAME];
    const uint8_t pixels[3] = {1, 2, 3};

    mm::buildColorLightRowPacket(packet, 0, 0, 1, pixels);

    const uint8_t dest[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const uint8_t src[6]  = {0x22, 0x22, 0x33, 0x44, 0x55, 0x66};
    CHECK(std::memcmp(packet, dest, 6) == 0);
    CHECK(std::memcmp(packet + 6, src, 6) == 0);
}

// The format overloads the EtherType field: byte 12 is the packet type and byte 13 is ALREADY the
// first payload byte. Pinning this is what stops the whole layout drifting one byte, which the
// cards answer with silence rather than an error.
TEST_CASE("Panel payload starts at byte 13, inside the EtherType field") {
    uint8_t packet[mm::COLORLIGHT_MAX_FRAME];
    const uint8_t pixels[6] = {255, 0, 128, 10, 20, 30};

    const size_t len = mm::buildColorLightRowPacket(packet, 5, 0, 2, pixels);

    CHECK(packet[12] == 0x55);   // packet type, where a normal frame's EtherType high byte sits
    CHECK(packet[13] == 0x00);   // row MSB — the first payload byte, in the EtherType's low half
    CHECK(packet[14] == 5);      // row LSB
    CHECK(packet[15] == 0x00);   // pixel offset
    CHECK(packet[16] == 0x00);
    CHECK(packet[17] == 0x00);   // pixel count
    CHECK(packet[18] == 2);
    CHECK(packet[19] == 0x08);   // the two constants the cards require
    CHECK(packet[20] == 0x88);
    CHECK(packet[21] == 255);    // pixel data starts at 21
    CHECK(packet[22] == 0);
    CHECK(packet[23] == 128);
    CHECK(packet[24] == 10);
    CHECK(len == mm::COLORLIGHT_ROW_PREFIX + 6);
}

// A row number above 255 spans both payload bytes, so a tall panel addresses its lower rows.
TEST_CASE("Panel row number above 255 is big-endian across two bytes") {
    uint8_t packet[mm::COLORLIGHT_MAX_FRAME];
    const uint8_t pixels[3] = {7, 8, 9};

    mm::buildColorLightRowPacket(packet, 300, 0, 1, pixels);   // 300 = 0x012C

    CHECK(packet[13] == 0x01);
    CHECK(packet[14] == 0x2C);
}

// A row wider than one packet splits, each carrying its own pixel offset so the card can place the
// chunk without depending on arrival order.
TEST_CASE("Panel row wider than one packet carries a pixel offset") {
    uint8_t packet[mm::COLORLIGHT_MAX_FRAME];
    static uint8_t pixels[mm::COLORLIGHT_MAX_PIXELS_PER_PACKET * 3] = {};

    const size_t len = mm::buildColorLightRowPacket(
        packet, 0, mm::COLORLIGHT_MAX_PIXELS_PER_PACKET, 3, pixels);

    CHECK(packet[15] == (mm::COLORLIGHT_MAX_PIXELS_PER_PACKET >> 8));
    CHECK(packet[16] == (mm::COLORLIGHT_MAX_PIXELS_PER_PACKET & 0xFF));
    CHECK(packet[18] == 3);
    CHECK(len == mm::COLORLIGHT_ROW_PREFIX + 9);
}

// A full-width packet is the largest frame the format builds, and it sizes the driver's one reused
// buffer — if this grew past COLORLIGHT_MAX_FRAME the driver would overrun it.
TEST_CASE("Panel full row packet fits the driver's frame buffer") {
    uint8_t packet[mm::COLORLIGHT_MAX_FRAME];
    static uint8_t pixels[mm::COLORLIGHT_MAX_PIXELS_PER_PACKET * 3] = {};

    const size_t len = mm::buildColorLightRowPacket(
        packet, 0, 0, mm::COLORLIGHT_MAX_PIXELS_PER_PACKET, pixels);

    CHECK(len == mm::COLORLIGHT_MAX_FRAME);
    CHECK(len == 1512);   // 21-byte prefix + 497 pixels x 3
}

// The sync frame latches everything sent since the last one. Fixed size, mostly zeros, with
// brightness repeated in the four places the cards read it.
TEST_CASE("Panel sync frame is 112 bytes and carries brightness") {
    uint8_t packet[mm::COLORLIGHT_SYNC_FRAME];

    const size_t len = mm::buildColorLightSyncPacket(packet, 200);

    CHECK(len == 112);
    CHECK(packet[12] == 0x01);   // sync packet type
    CHECK(packet[13] == 0x07);   // sent from a PC/netcard rather than a hardware sender
    CHECK(packet[35] == 200);    // payload byte 22
    CHECK(packet[36] == 0x05);
    CHECK(packet[38] == 200);    // payload bytes 25-27
    CHECK(packet[39] == 200);
    CHECK(packet[40] == 200);
}

// The brightness frame sets the card's own gain, ahead of the row data.
TEST_CASE("Panel brightness frame is 77 bytes on three channels") {
    uint8_t packet[mm::COLORLIGHT_BRIGHTNESS_FRAME];

    const size_t len = mm::buildColorLightBrightnessPacket(packet, 128);

    CHECK(len == 77);
    CHECK(packet[12] == 0x0A);   // brightness packet type
    CHECK(packet[13] == 128);
    CHECK(packet[14] == 128);
    CHECK(packet[15] == 128);
    CHECK(packet[16] == 0xFF);
}
