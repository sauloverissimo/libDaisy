#pragma once
#ifndef DSY_MIDI2_PROCESSOR_H
#define DSY_MIDI2_PROCESSOR_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif
#include "hid/midi2/midi2_conv.h"
#include "hid/midi2/midi2_proc.h"
#include "hid/midi2/midi2_ci.h"
#include "hid/midi2/midi2_dispatch.h"
#ifdef __cplusplus
}
#endif

namespace daisy
{
/** @brief   MIDI 2.0 processor: byte stream to UMP dispatch + MIDI-CI
 *  @details Bridges the libDaisy transport layer with the midi2 library.
 *           Converts incoming MIDI 1.0 bytes to UMP, reassembles SysEx,
 *           auto-responds to MIDI-CI Discovery, and dispatches typed
 *           UMP callbacks for the application.
 *
 *  @note    This is opt-in. Existing MIDI 1.0 sketches are unaffected.
 *           To enable, create a Midi2Processor, Init() it, configure
 *           callbacks on GetDispatch(), and pass it to MidiHandler::EnableMidi2().
 *
 *  @ingroup midi
 */
class Midi2Processor
{
  public:
    /** Callback type for sending raw bytes back through the transport */
    typedef void (*TxCallback)(uint8_t* data, size_t size, void* context);

    /** Configuration for MIDI 2.0 support */
    struct Config
    {
        /** 3-byte SysEx Manufacturer ID (lower 24 bits).
         *  Required for MIDI-CI Discovery responses. */
        uint32_t manufacturer_id;

        uint16_t family_id;
        uint16_t model_id;
        uint32_t version_id;

        /** Seed for MUID generation (28-bit). Use a random or unique value. */
        uint32_t muid_seed;

        /** UMP group for byte-to-UMP conversion (0-15) */
        uint8_t group;

        /** When true, the convenience CI responder replies NAK to any
         *  CI command it does not understand, per M2-101-UM Appendix E.
         *  When false, unknown CI commands are silently ignored.
         *  Default true (spec recommendation). */
        bool nak_on_unknown;

        Config()
        : manufacturer_id(0),
          family_id(0),
          model_id(0),
          version_id(0),
          muid_seed(0),
          group(0),
          nak_on_unknown(true)
        {
        }
    };

    Midi2Processor() : tx_cb_(nullptr), tx_ctx_(nullptr), tx_len_(0) {}
    ~Midi2Processor() {}

    /** Initialize all midi2 subsystems */
    void Init(Config config);

    /** Set the Tx callback (called by MidiHandler::EnableMidi2) */
    void SetTx(TxCallback cb, void* context)
    {
        tx_cb_  = cb;
        tx_ctx_ = context;
    }

    /** Set the random source for MUID generation and collision recovery.
     *  Optional. When unset, the MUID stays at the deterministic value
     *  derived from Config::muid_seed (fine for a single device on the bus,
     *  fragile when several Daisy devices coexist).
     *
     *  Typical Daisy wiring: call this from setup() after the user has
     *  enabled the STM32H7 RNG peripheral, with a small wrapper that
     *  returns 32 bits from HAL_RNG_GenerateRandomNumber. Any other 32-bit
     *  entropy source works too (audio noise, ADC LSBs, unique chip id +
     *  timer, etc).
     *
     *  @param rng     callback returning 32 random bits
     *  @param context user pointer passed to the callback */
    void SetRng(midi2_ci_rng_fn rng, void* context)
    {
        midi2_ci_set_rng(&ci_, rng, context);
    }

    /** Feed one incoming MIDI 1.0 byte.
     *  Converts to UMP, dispatches to callbacks, handles CI. */
    void Feed(uint8_t byte);

    /** Feed UMP words directly (from USB MIDI 2.0 Alt 1).
     *  Bypasses midi2_conv, goes straight to midi2_proc. */
    void FeedUmp(const uint32_t* words, uint8_t word_count);

    /** Send UMP words through the transport (for Alt 1 Tx). */
    void SendUmp(const uint32_t* words, uint8_t word_count);

    /** Access the UMP dispatch struct to register callbacks.
     *  Example:
     *    proc.GetDispatch()->on_note_on = my_handler;
     *    proc.GetDispatch()->context = &my_app; */
    midi2_dispatch* GetDispatch() { return &dispatch_; }

    /** Access the CI state for adding profiles or properties */
    midi2_ci_state* GetCI() { return &ci_; }

  private:
    /* midi2 state */
    midi2_conv_state conv_;
    midi2_proc_state proc_;
    midi2_ci_state   ci_;
    midi2_dispatch   dispatch_;

    /* Transport Tx bridge */
    TxCallback tx_cb_;
    void*      tx_ctx_;

    /* Buffers */
    static constexpr size_t kSysexBufSize = 256;
    uint8_t                 proc_sysex7_buf_[kSysexBufSize];
    uint8_t                 ci_profiles_buf_[4][5];

    /* Tx accumulation buffer for CI SysEx responses */
    static constexpr size_t kTxBufSize = 512;
    uint8_t                 tx_buf_[kTxBufSize];
    size_t                  tx_len_;

    /* Internal static callbacks (wiring between midi2 components) */

    /** Called by midi2_proc when a complete UMP passes the group filter.
     *  Routes to midi2_dispatch for typed callbacks. */
    static void OnUmp(const uint32_t* words, uint8_t count, void* ctx);

    /** Called by midi2_proc when a complete SysEx7 is reassembled.
     *  Routes to midi2_ci for auto-response, then to user CI callbacks. */
    static void
    OnSysex7(uint8_t group, const uint8_t* data, uint16_t length, void* ctx);

    /** Called by midi2_ci when sending CI SysEx responses.
     *  Converts UMP SysEx7 packets back to MIDI 1.0 byte stream
     *  and sends through the transport via tx_cb_. */
    static uint32_t CiWriteFn(const uint32_t* words, uint32_t count, void* ctx);

    /** UMP Stream auto-responders (wired in Init to dispatch) */
    static void OnEndpointDiscovery(uint8_t ump_ver_major,
                                    uint8_t ump_ver_minor,
                                    uint8_t filter,
                                    void*   ctx);
    static void
    OnConfigRequest(uint8_t protocol, bool rx_jr, bool tx_jr, void* ctx);
    static void OnFbDiscovery(uint8_t fb_num, uint8_t filter, void* ctx);
};

} // namespace daisy

#endif // DSY_MIDI2_PROCESSOR_H
