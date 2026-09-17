#pragma once
/// MPEG-TS muxing: H.264 access units into the 188-byte transport packets an HLS segment is
/// made of. Written for the ESP32-P4 HLS path, where the hardware encoder hands us Annex-B NALs
/// and nothing else does the packaging (desktop hands the whole job to ffmpeg).
///
/// **Pure logic, no platform calls**, so the packet structure is pinned by host unit tests rather
/// than only on a P4: feed it canned NALs, assert the bytes. That is the entire reason this is a
/// header of its own and not folded into the encoder file.
///
/// Scope is exactly what an HLS segment of our own stream needs: one video program, one H.264
/// elementary stream, no audio, no PCR-only packets. MPEG-TS is ISO/IEC 13818-1; the H.264-in-TS
/// mapping is its Annex, and the byte layout below follows them directly.
///
/// Author: projectMM original

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::ts {

/// One transport packet. Everything in MPEG-TS is this size, always.
static constexpr size_t kPacketSize = 188;

/// The PIDs we emit. PAT is fixed at 0 by the standard; the rest are our choice and match what
/// ffmpeg's muxer uses, so a stream from either source looks the same to a player.
static constexpr uint16_t kPidPat   = 0x0000;
static constexpr uint16_t kPidPmt   = 0x1000;
static constexpr uint16_t kPidVideo = 0x0100;

/// The 90 kHz clock every TS timestamp is counted in (ISO/IEC 13818-1).
static constexpr uint32_t kClockHz = 90000;

/// The continuity counters, which belong to the STREAM rather than to any one write. MPEG-TS
/// requires each PID's counter to advance by one per packet without interruption, across frames
/// and across segment boundaries alike: a player treats any gap as lost packets. The caller keeps
/// one of these for the whole stream and hands it to every Writer.
struct Continuity {
    uint8_t pat = 0, pmt = 0, video = 0;
};

/// Writes packets into a caller-owned buffer, reporting overflow rather than growing: a segment
/// buffer is a fixed PSRAM slot on the P4, and a muxer that allocates would be the one thing in
/// the hot path doing so.
class Writer {
public:
    /// `cc` must outlive the Writer and be the SAME object for every write in a stream: a Writer
    /// is created per frame, and per-Writer counters would restart at zero on each one (bench:
    /// ffmpeg reported "Packet corrupt" at every frame, and players stalled and re-buffered).
    Writer(uint8_t* dst, size_t cap, Continuity& cc) : dst_(dst), cap_(cap), cc_(cc) {}

    /// Bytes written so far.
    size_t size() const { return len_; }
    /// Did any write not fit? Once true the output is incomplete and must be discarded.
    bool overflowed() const { return overflow_; }

    /// Start a segment: PAT then PMT, so a player tuning in mid-stream can decode from the first
    /// packet it sees. HLS players join at segment boundaries, so every segment repeats them.
    void writeTables() {
        writeTable(kPidPat, patPayload, sizeof(patPayload), cc_.pat);
        writeTable(kPidPmt, pmtPayload, sizeof(pmtPayload), cc_.pmt);
    }

    /// Write one access unit (the NALs of a single frame, Annex-B start codes included) as a PES
    /// packet split across as many TS packets as it needs.
    ///
    /// `pts90` is the presentation time in 90 kHz ticks. We emit PTS only, never a separate DTS:
    /// the encoder produces no B-frames, so decode and presentation order are identical and the
    /// standard's own rule is to omit DTS when it would equal PTS. `keyframe` marks an IDR, which
    /// gets the PCR (a player needs a clock reference at the point it can start decoding).
    void writeAccessUnit(const uint8_t* au, size_t len, uint32_t pts90, bool keyframe) {
        if (!au || len == 0) return;

        // PES header: start code, stream id 0xE0 (video), then a length that we deliberately
        // leave 0. A video PES may legally declare "unbounded" this way, which is what lets a
        // frame exceed 65535 bytes -- routine at wall resolutions -- and is what ffmpeg emits too.
        uint8_t pes[20];   // 9-byte PES header + 5-byte PTS + the 6-byte access unit delimiter
        size_t p = 0;
        pes[p++] = 0x00; pes[p++] = 0x00; pes[p++] = 0x01; pes[p++] = 0xE0;
        pes[p++] = 0x00; pes[p++] = 0x00;                  // length: unbounded
        pes[p++] = 0x80;                                   // '10' marker, no scrambling
        pes[p++] = 0x80;                                   // PTS present, DTS absent
        pes[p++] = 5;                                      // PTS is 5 bytes
        writePts(pes + p, pts90);
        p += 5;

        // An access unit delimiter tells the decoder where a frame starts without it having to
        // infer boundaries from slice headers. ffmpeg inserts one too.
        static const uint8_t aud[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};
        std::memcpy(pes + p, aud, sizeof(aud));
        p += sizeof(aud);

        bool first = true;
        size_t pesOff = 0, auOff = 0;
        while (pesOff < p || auOff < len) {
            uint8_t* pkt = claim();
            if (!pkt) return;

            // Payload room is what is left after the 4-byte header and any adaptation field. The
            // FIRST packet of a frame carries the payload-unit-start flag, and a keyframe's first
            // packet also carries the PCR, so both live in the same branch.
            size_t avail = kPacketSize - 4;
            uint8_t adaptLen = 0;
            const bool wantPcr = first && keyframe;
            if (wantPcr) adaptLen = 1 + 1 + 6;             // length byte + flags + 48-bit PCR

            const size_t remaining = (p - pesOff) + (len - auOff);
            // A short tail must be padded out with a stuffing adaptation field: TS packets are
            // never partially filled.
            if (remaining + adaptLen < avail) {
                const size_t need = avail - remaining;
                adaptLen = static_cast<uint8_t>(adaptLen ? adaptLen + need : (need >= 1 ? need : 1));
            }

            pkt[0] = 0x47;                                 // sync byte, every packet
            pkt[1] = static_cast<uint8_t>((first ? 0x40 : 0x00) | ((kPidVideo >> 8) & 0x1F));
            pkt[2] = static_cast<uint8_t>(kPidVideo & 0xFF);
            pkt[3] = static_cast<uint8_t>((adaptLen ? 0x30 : 0x10) | (cc_.video & 0x0F));
            cc_.video++;

            size_t o = 4;
            if (adaptLen) {
                pkt[o++] = static_cast<uint8_t>(adaptLen - 1);   // length excludes itself
                if (adaptLen >= 2) {
                    pkt[o++] = wantPcr ? 0x50 : 0x00;      // random-access + PCR flags
                    if (wantPcr) { writePcr(pkt + o, pts90); o += 6; }
                    // Remaining adaptation bytes are stuffing, which must be 0xFF.
                    const size_t stuffEnd = 4 + adaptLen;
                    while (o < stuffEnd) pkt[o++] = 0xFF;
                }
                avail = kPacketSize - o;
            }

            // PES header first, then frame bytes, until the packet is full.
            while (o < kPacketSize && pesOff < p) pkt[o++] = pes[pesOff++];
            const size_t take = kPacketSize - o < len - auOff ? kPacketSize - o : len - auOff;
            if (take) { std::memcpy(pkt + o, au + auOff, take); auOff += take; o += take; }
            while (o < kPacketSize) pkt[o++] = 0xFF;       // only reachable when nothing is left

            first = false;
        }
    }

private:
    // PAT: one program (number 1) whose map lives on kPidPmt.
    static constexpr uint8_t patPayload[] = {
        0x00,                    // table id: program association
        0xB0, 0x0D,              // section syntax indicator + length 13
        0x00, 0x01,              // transport stream id
        0xC1,                    // version 0, current
        0x00, 0x00,              // section 0 of 0
        0x00, 0x01,              // program number 1
        // The PMT's PID, with the top 3 reserved bits set: 0b111 | 0x1000 = 0xF0 0x00. Getting
        // this wrong points the PAT at PID 0 instead, and a player that trusts the PAT (VLC does;
        // ffmpeg probes for streams anyway) never finds the PMT, so it sees no video track at all.
        0xF0, 0x00,              // -> kPidPmt (0x1000)
        0x00, 0x00, 0x00, 0x00,  // CRC, filled in by writeTable
    };
    // PMT: one elementary stream, type 0x1B (H.264), which is also the PCR PID.
    static constexpr uint8_t pmtPayload[] = {
        0x02,                    // table id: program map
        0xB0, 0x12,              // section syntax indicator + length 18
        0x00, 0x01,              // program number
        0xC1,                    // version 0, current
        0x00, 0x00,              // section 0 of 0
        0xE1, 0x00,              // PCR carried on the video PID
        0xF0, 0x00,              // no program info
        0x1B,                    // stream type: H.264
        0xE1, 0x00,              // elementary PID = kPidVideo
        0xF0, 0x00,              // no ES info
        0x00, 0x00, 0x00, 0x00,  // CRC
    };

    /// Reserve one packet's worth of output, or report overflow.
    uint8_t* claim() {
        if (len_ + kPacketSize > cap_) { overflow_ = true; return nullptr; }
        uint8_t* p = dst_ + len_;
        len_ += kPacketSize;
        return p;
    }

    /// A PSI table is one packet: pointer byte, the section, CRC, then 0xFF to the end.
    void writeTable(uint16_t pid, const uint8_t* section, size_t sectionLen, uint8_t& cc) {
        uint8_t* pkt = claim();
        if (!pkt) return;
        pkt[0] = 0x47;
        pkt[1] = static_cast<uint8_t>(0x40 | ((pid >> 8) & 0x1F));   // payload starts here
        pkt[2] = static_cast<uint8_t>(pid & 0xFF);
        pkt[3] = static_cast<uint8_t>(0x10 | (cc & 0x0F));
        cc++;
        pkt[4] = 0x00;                                    // pointer field: section starts next
        std::memcpy(pkt + 5, section, sectionLen);
        // The CRC covers the section from its table id up to (not including) the CRC itself.
        const uint32_t crc = crc32Mpeg(pkt + 5, sectionLen - 4);
        uint8_t* c = pkt + 5 + sectionLen - 4;
        c[0] = static_cast<uint8_t>(crc >> 24); c[1] = static_cast<uint8_t>(crc >> 16);
        c[2] = static_cast<uint8_t>(crc >> 8);  c[3] = static_cast<uint8_t>(crc);
        std::memset(pkt + 5 + sectionLen, 0xFF, kPacketSize - 5 - sectionLen);
    }

    /// PTS in the standard's split-across-marker-bits layout (13818-1 table 2-21).
    static void writePts(uint8_t* d, uint32_t pts) {
        d[0] = static_cast<uint8_t>(0x21 | ((pts >> 29) & 0x0E));
        d[1] = static_cast<uint8_t>((pts >> 22) & 0xFF);
        d[2] = static_cast<uint8_t>(0x01 | ((pts >> 14) & 0xFE));
        d[3] = static_cast<uint8_t>((pts >> 7) & 0xFF);
        d[4] = static_cast<uint8_t>(0x01 | ((pts << 1) & 0xFE));
    }

    /// PCR: a 33-bit base at 90 kHz plus a 9-bit extension we leave at zero (we have no finer
    /// clock to report, and players only need the base).
    static void writePcr(uint8_t* d, uint32_t pcr) {
        d[0] = static_cast<uint8_t>(pcr >> 25);
        d[1] = static_cast<uint8_t>(pcr >> 17);
        d[2] = static_cast<uint8_t>(pcr >> 9);
        d[3] = static_cast<uint8_t>(pcr >> 1);
        d[4] = static_cast<uint8_t>(((pcr & 1) << 7) | 0x7E);
        d[5] = 0x00;
    }

    /// CRC-32/MPEG-2: the PSI section checksum (polynomial 0x04C11DB7, MSB-first, no final xor).
    static uint32_t crc32Mpeg(const uint8_t* data, size_t len) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; i++) {
            crc ^= static_cast<uint32_t>(data[i]) << 24;
            for (int b = 0; b < 8; b++)
                crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
        return crc;
    }

    uint8_t*     dst_;
    size_t       cap_;
    Continuity&  cc_;
    size_t       len_ = 0;
    bool         overflow_ = false;
};

}  // namespace mm::ts
