#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "hid/midi2_processor.h"

using namespace daisy;

// ================ Test Fixtures ================

class Midi2ProcessorTest : public ::testing::Test
{
  public:
    void SetUp() override
    {
        Midi2Processor::Config cfg;
        cfg.manufacturer_id = 0x7D;
        cfg.family_id       = 0x0001;
        cfg.model_id        = 0x0001;
        cfg.version_id      = 0x00010000;
        cfg.muid_seed       = 0xDA15;
        cfg.group           = 0;
        proc.Init(cfg);

        // upscale MIDI 1.0 (MT 0x2) to MIDI 2.0 (MT 0x4) callbacks
        proc.GetDispatch()->upscale_mt2 = true;

        tx_data.clear();
        proc.SetTx(TxCapture, this);

        note_on_count  = 0;
        note_off_count = 0;
        cc_count       = 0;
        last_note      = 0;
        last_vel       = 0;
        last_channel   = 0;
        last_cc_index  = 0;
        last_cc_value  = 0;
    }

    static void TxCapture(uint8_t* data, size_t size, void* context)
    {
        auto* self = static_cast<Midi2ProcessorTest*>(context);
        self->tx_data.insert(
            self->tx_data.end(), data, data + size);
    }

    Midi2Processor       proc;
    std::vector<uint8_t> tx_data;

    // Callback counters
    uint32_t note_on_count;
    uint32_t note_off_count;
    uint32_t cc_count;
    uint8_t  last_note;
    uint16_t last_vel;
    uint8_t  last_channel;
    uint8_t  last_cc_index;
    uint32_t last_cc_value;
};

// ================ Callback Helpers ================

static void OnNoteOnTest(uint8_t  group,
                         uint8_t  channel,
                         uint8_t  note,
                         uint16_t velocity,
                         uint8_t  attr_type,
                         uint16_t attr_data,
                         void*    ctx)
{
    (void)group;
    (void)attr_type;
    (void)attr_data;
    auto* self         = static_cast<Midi2ProcessorTest*>(ctx);
    self->note_on_count++;
    self->last_note    = note;
    self->last_vel     = velocity;
    self->last_channel = channel;
}

static void OnNoteOffTest(uint8_t  group,
                          uint8_t  channel,
                          uint8_t  note,
                          uint16_t velocity,
                          uint8_t  attr_type,
                          uint16_t attr_data,
                          void*    ctx)
{
    (void)group;
    (void)velocity;
    (void)attr_type;
    (void)attr_data;
    auto* self = static_cast<Midi2ProcessorTest*>(ctx);
    self->note_off_count++;
    self->last_note    = note;
    self->last_channel = channel;
}

static void OnCCTest(uint8_t  group,
                     uint8_t  channel,
                     uint8_t  index,
                     uint32_t value,
                     void*    ctx)
{
    (void)group;
    (void)channel;
    auto* self        = static_cast<Midi2ProcessorTest*>(ctx);
    self->cc_count++;
    self->last_cc_index = index;
    self->last_cc_value = value;
}

// ================ Init ================

TEST_F(Midi2ProcessorTest, initDoesNotCrash)
{
    // Init already called in SetUp -- just verify dispatch is accessible
    EXPECT_NE(proc.GetDispatch(), nullptr);
    EXPECT_NE(proc.GetCI(), nullptr);
}

TEST_F(Midi2ProcessorTest, dispatchCallbacksAreNullByDefault)
{
    midi2_dispatch* dp = proc.GetDispatch();
    EXPECT_EQ(dp->on_note_on, nullptr);
    EXPECT_EQ(dp->on_note_off, nullptr);
    EXPECT_EQ(dp->on_cc, nullptr);
}

// ================ Feed: MIDI 1.0 byte -> UMP dispatch ================

TEST_F(Midi2ProcessorTest, feedNoteOnFromBytes)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_note_on     = OnNoteOnTest;
    dp->context        = this;

    // MIDI 1.0 NoteOn: channel 0, note 60, velocity 100
    proc.Feed(0x90);
    proc.Feed(60);
    proc.Feed(100);

    EXPECT_EQ(note_on_count, 1u);
    EXPECT_EQ(last_note, 60);
    EXPECT_EQ(last_channel, 0);
}

TEST_F(Midi2ProcessorTest, feedNoteOffFromBytes)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_note_off    = OnNoteOffTest;
    dp->context        = this;

    // MIDI 1.0 NoteOff: channel 3, note 72
    proc.Feed(0x83);
    proc.Feed(72);
    proc.Feed(64);

    EXPECT_EQ(note_off_count, 1u);
    EXPECT_EQ(last_note, 72);
    EXPECT_EQ(last_channel, 3);
}

TEST_F(Midi2ProcessorTest, feedCCFromBytes)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_cc          = OnCCTest;
    dp->context        = this;

    // MIDI 1.0 CC: channel 0, cc 7 (volume), value 100
    proc.Feed(0xB0);
    proc.Feed(7);
    proc.Feed(100);

    EXPECT_EQ(cc_count, 1u);
    EXPECT_EQ(last_cc_index, 7);
}

TEST_F(Midi2ProcessorTest, feedMultipleMessages)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_note_on     = OnNoteOnTest;
    dp->on_note_off    = OnNoteOffTest;
    dp->context        = this;

    // NoteOn ch0 note 60
    proc.Feed(0x90);
    proc.Feed(60);
    proc.Feed(100);
    // NoteOff ch0 note 60
    proc.Feed(0x80);
    proc.Feed(60);
    proc.Feed(64);
    // NoteOn ch0 note 64
    proc.Feed(0x90);
    proc.Feed(64);
    proc.Feed(80);

    EXPECT_EQ(note_on_count, 2u);
    EXPECT_EQ(note_off_count, 1u);
}

// ================ Feed: upscale_mt2 ================

TEST_F(Midi2ProcessorTest, upscaleMt2ScalesVelocity)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->upscale_mt2    = true;
    dp->on_note_on     = OnNoteOnTest;
    dp->context        = this;

    // MIDI 1.0 NoteOn: velocity 127 (7-bit max)
    proc.Feed(0x90);
    proc.Feed(60);
    proc.Feed(127);

    EXPECT_EQ(note_on_count, 1u);
    // upscale 7->16: 127 should become 0xFFFF
    EXPECT_EQ(last_vel, 0xFFFF);
}

TEST_F(Midi2ProcessorTest, upscaleMt2ScalesCC)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->upscale_mt2    = true;
    dp->on_cc          = OnCCTest;
    dp->context        = this;

    // MIDI 1.0 CC: value 127 (7-bit max)
    proc.Feed(0xB0);
    proc.Feed(7);
    proc.Feed(127);

    EXPECT_EQ(cc_count, 1u);
    // upscale 7->32: 127 should become 0xFFFFFFFF
    EXPECT_EQ(last_cc_value, 0xFFFFFFFFu);
}

// ================ FeedUmp: native UMP path ================

TEST_F(Midi2ProcessorTest, feedUmpNoteOn)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_note_on     = OnNoteOnTest;
    dp->context        = this;

    // Build a MIDI 2.0 NoteOn UMP (MT 0x4)
    uint32_t w[2];
    midi2_msg_note_on(w, 0, 5, 72, 0xC000, 0);
    proc.FeedUmp(w, 2);

    EXPECT_EQ(note_on_count, 1u);
    EXPECT_EQ(last_note, 72);
    EXPECT_EQ(last_vel, 0xC000);
    EXPECT_EQ(last_channel, 5);
}

TEST_F(Midi2ProcessorTest, feedUmpCC)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_cc          = OnCCTest;
    dp->context        = this;

    uint32_t w[2];
    midi2_msg_cc(w, 0, 0, 74, 0x80000000);
    proc.FeedUmp(w, 2);

    EXPECT_EQ(cc_count, 1u);
    EXPECT_EQ(last_cc_index, 74);
    EXPECT_EQ(last_cc_value, 0x80000000u);
}

// ================ CI identity ================

TEST_F(Midi2ProcessorTest, ciIdentityIsSet)
{
    midi2_ci_state* ci = proc.GetCI();
    EXPECT_EQ(ci->manufacturer_id, 0x7Du);
    EXPECT_EQ(ci->family_id, 0x0001u);
    EXPECT_EQ(ci->model_id, 0x0001u);
    EXPECT_EQ(ci->version_id, 0x00010000u);
}

// ================ SendUmp triggers Tx callback ================

TEST_F(Midi2ProcessorTest, sendUmpTriggersTx)
{
    uint32_t w[2];
    midi2_msg_note_on(w, 0, 0, 60, 0x8000, 0);
    proc.SendUmp(w, 2);

    EXPECT_EQ(tx_data.size(), 8u); // 2 words * 4 bytes
}

// ================ Running status through Feed ================

TEST_F(Midi2ProcessorTest, runningStatusWorks)
{
    midi2_dispatch* dp = proc.GetDispatch();
    dp->on_note_on     = OnNoteOnTest;
    dp->context        = this;

    // NoteOn status
    proc.Feed(0x90);
    proc.Feed(60);
    proc.Feed(100);
    // Running status: same status, new data
    proc.Feed(64);
    proc.Feed(80);

    EXPECT_EQ(note_on_count, 2u);
    EXPECT_EQ(last_note, 64);
}
