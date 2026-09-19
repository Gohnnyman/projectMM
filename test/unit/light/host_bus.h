#pragma once

/// @defgroup host_bus The host-bus contract
/// @{
/// What every parallel peripheral must do off-device, asserted once and called per type.
///
/// @moreinfo
///
/// Three DMA peripherals do one job, and off-device that job is holding a frame: the desktop backs all three with the same heap buffer.
/// So the assertions are identical per peripheral, and writing them per file produced two byte-identical test cases differing only in a type name.
///
/// ## Why the buffers must appear in the memory readout
///
/// They are allocated by the platform rather than by the driver, and were once left out of the driver's own figure on that ownership argument.
/// That made the card lie about the one thing a user picks a driver on.
/// An i80 frame is sized by the bus width rather than the pins in use, so a one-lane board pays what an eight-lane one does.
///
/// Measured on the bench, a one-lane board read 512 bytes for this driver against another's 32 KB.
/// It actually cost 49 KB more free heap, and nothing on screen said so.

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"

#include <cstdint>

namespace mm::test {

/// Pin the memory-backed bus contract for one peripheral type.
template <typename Peripheral>
inline void checkHostBusAllocates() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);              // busInit reads the pin list through the owner
    drv.setPeripheralForTest(&peripheral);  // borrowed, not owned, so no delete of a local

    REQUIRE(peripheral.busInit(768, /*wantSecondBuffer=*/false));
    CHECK(peripheral.busCapacity() == 768);

    uint8_t* buf = peripheral.busBuffer(0);
    REQUIRE(buf != nullptr);
    // Writable and reading back: a bus that carries the frame rather than discarding it.
    buf[0] = 0xA5;
    buf[767] = 0x5A;
    CHECK(buf[0] == 0xA5);
    CHECK(buf[767] == 0x5A);

    CHECK(peripheral.busTransmit(0, 768));
    // Truncating would look like a good frame while dropping every light past the end.
    CHECK_FALSE(peripheral.busTransmit(0, 4096));
    // A false here reads as an incomplete frame and stalls a bus that is never busy.
    CHECK(peripheral.busWait(0, 10));

    peripheral.busDeinit();
    CHECK(peripheral.busBuffer(0) == nullptr);
}

/// The DMA buffers must show up in the driver's memory readout.
template <typename Peripheral>
inline void checkHostBusCountedInHeapReadout() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);

    // Through the public readout the card renders, which is the number this test exists to pin.
    drv.publishHeapBytesForTest();
    const size_t before = drv.dynamicBytes();

    REQUIRE(peripheral.busInit(4096, /*wantSecondBuffer=*/false));
    drv.publishHeapBytesForTest();
    const size_t single = drv.dynamicBytes();
    CHECK(single >= before + 4096);   // the frame is now visible, whoever allocated it

    // Counting it is what tells a user the second buffer costs a whole extra frame.
    REQUIRE(peripheral.busInit(4096, /*wantSecondBuffer=*/true));
    drv.doubleBuffer = true;
    drv.publishHeapBytesForTest();
    const size_t doubled = drv.dynamicBytes();
    CHECK(doubled == single + 4096);   // exactly one extra frame, not a re-count of the whole driver

    peripheral.busDeinit();
}

/// Double-buffering lets the driver encode one frame while the other is in flight, so the two must be distinct allocations, since aliasing them would tear every frame.
template <typename Peripheral>
inline void checkHostBusDoubleBuffer() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);

    REQUIRE(peripheral.busInit(256, /*wantSecondBuffer=*/true));
    REQUIRE(peripheral.busBuffer(0) != nullptr);
    REQUIRE(peripheral.busBuffer(1) != nullptr);
    CHECK(peripheral.busBuffer(0) != peripheral.busBuffer(1));

    // Re-initialising releases the second, rather than leaving a stale span to read from.
    REQUIRE(peripheral.busInit(256, /*wantSecondBuffer=*/false));
    CHECK(peripheral.busBuffer(1) == nullptr);
}

/// @}
}  // namespace mm::test
