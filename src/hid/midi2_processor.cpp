#include "hid/midi2_processor.h"
#include <cstring>

using namespace daisy;

/*--------------------------------------------------------------------+
 * Init -- wire all midi2 components together
 *--------------------------------------------------------------------*/
void Midi2Processor::Init(Config config)
{
    tx_len_ = 0;

    /* Byte stream -> UMP converter (6-byte internal SysEx buffer) */
    midi2_conv_init(&conv_, config.group);

    /* UMP processor: group filter + SysEx7 reassembly */
    midi2_proc_init(&proc_,
                    proc_sysex7_buf_,
                    kSysexBufSize,
                    nullptr,
                    0); /* no SysEx8 over byte stream */
    proc_.on_ump    = Midi2Processor::OnUmp;
    proc_.on_sysex7 = Midi2Processor::OnSysex7;
    proc_.context   = this;

    /* MIDI-CI auto-responder */
    midi2_ci_init(&ci_,
                  config.muid_seed,
                  ci_profiles_buf_,
                  4,
                  nullptr,
                  0); /* properties added later by user */
    midi2_ci_set_identity(&ci_,
                          config.manufacturer_id,
                          config.family_id,
                          config.model_id,
                          config.version_id);
    midi2_ci_set_write_fn(&ci_, Midi2Processor::CiWriteFn, this);

    /* UMP typed dispatch */
    midi2_dispatch_init(&dispatch_);

    /* Wire UMP Stream auto-responder callbacks */
    dispatch_.on_endpoint_discovery = Midi2Processor::OnEndpointDiscovery;
    dispatch_.on_config_request     = Midi2Processor::OnConfigRequest;
    dispatch_.on_fb_discovery       = Midi2Processor::OnFbDiscovery;
    dispatch_.context               = this;
}

/*--------------------------------------------------------------------+
 * Feed -- one byte at a time from the transport
 *
 * midi2_conv converts each byte to UMP (streaming SysEx7 included).
 * midi2_proc reassembles SysEx7 and dispatches UMP to callbacks.
 * MIDI-CI is handled via the reassembled SysEx7 in OnSysex7.
 *--------------------------------------------------------------------*/
void Midi2Processor::Feed(uint8_t byte)
{
    if(midi2_conv_feed(&conv_, byte))
    {
        midi2_proc_feed(&proc_, conv_.ump, conv_.ump_words);
    }
}

/*--------------------------------------------------------------------+
 * FeedUmp -- UMP words from native transport (bypasses midi2_conv)
 *--------------------------------------------------------------------*/
void Midi2Processor::FeedUmp(const uint32_t* words, uint8_t word_count)
{
    midi2_proc_feed(&proc_, words, word_count);
}

/*--------------------------------------------------------------------+
 * SendUmp -- send UMP words through the transport
 *--------------------------------------------------------------------*/
void Midi2Processor::SendUmp(const uint32_t* words, uint8_t word_count)
{
    if(tx_cb_)
    {
        tx_cb_((uint8_t*)words, word_count * 4, tx_ctx_);
    }
}

/*--------------------------------------------------------------------+
 * OnUmp -- UMP passes group filter, dispatch to typed callbacks
 *--------------------------------------------------------------------*/
void Midi2Processor::OnUmp(const uint32_t* words, uint8_t count, void* ctx)
{
    Midi2Processor* self = static_cast<Midi2Processor*>(ctx);
    midi2_dispatch_feed(words, count, &self->dispatch_);
}

/*--------------------------------------------------------------------+
 * UMP Stream auto-responders (Endpoint Discovery, Config, FB)
 *--------------------------------------------------------------------*/
void Midi2Processor::OnEndpointDiscovery(uint8_t ump_ver_major,
                                         uint8_t ump_ver_minor,
                                         uint8_t filter,
                                         void*   ctx)
{
    Midi2Processor* self = static_cast<Midi2Processor*>(ctx);
    uint32_t        w[4];

    /* Endpoint Info Reply */
    midi2_msg_stream_endpoint_info(
        w, 0x01, 0x01, false, 1, true, true, false, false);
    self->SendUmp(w, 4);

    /* Device Identity */
    midi2_msg_stream_device_identity(w,
                                     self->ci_.manufacturer_id,
                                     self->ci_.family_id,
                                     self->ci_.model_id,
                                     self->ci_.version_id);
    self->SendUmp(w, 4);
}

void Midi2Processor::OnConfigRequest(uint8_t protocol,
                                     bool    rx_jr,
                                     bool    tx_jr,
                                     void*   ctx)
{
    Midi2Processor* self = static_cast<Midi2Processor*>(ctx);
    uint32_t        w[4];

    /* Accept whatever protocol the host requests */
    midi2_msg_stream_config_notify(w, protocol);
    self->SendUmp(w, 4);
}

void Midi2Processor::OnFbDiscovery(uint8_t fb_num, uint8_t filter, void* ctx)
{
    Midi2Processor* self = static_cast<Midi2Processor*>(ctx);
    uint32_t        w[4];

    /* 1 function block, bidirectional, group 0, 1 group, auto protocol */
    midi2_msg_stream_fb_info(w, true, 0, 0x00, 0, 1, 0x01, false, 0x00);
    self->SendUmp(w, 4);
}

/*--------------------------------------------------------------------+
 * OnSysex7 -- reassembled SysEx: try CI first, then user dispatch
 *--------------------------------------------------------------------*/
void Midi2Processor::OnSysex7(uint8_t        group,
                              const uint8_t* data,
                              uint16_t       length,
                              void*          ctx)
{
    Midi2Processor* self = static_cast<Midi2Processor*>(ctx);

    /* Let the CI auto-responder handle it if it's a CI message */
    midi2_ci_process_sysex(&self->ci_, group, data, length);

    /* The UMP dispatch also sees it (on_sysex7 callback) via the
     * normal midi2_proc on_ump path, so the user can still register
     * a sysex7 callback on the dispatch if they want raw access. */
}

/*--------------------------------------------------------------------+
 * CiWriteFn -- convert UMP SysEx7 packets back to MIDI 1.0 byte stream
 *
 * Called by midi2_ci via midi2_proc_send_sysex7() when sending CI
 * responses. Each call delivers 2 UMP words (one SysEx7 packet).
 * We extract the data bytes, wrap in F0/F7, and send via transport.
 *--------------------------------------------------------------------*/
uint32_t
Midi2Processor::CiWriteFn(const uint32_t* words, uint32_t count, void* ctx)
{
    Midi2Processor* self = static_cast<Midi2Processor*>(ctx);
    uint32_t        sent = 0;

    for(uint32_t i = 0; i < count; i += 2)
    {
        uint32_t w0 = words[i];
        uint32_t w1 = (i + 1 < count) ? words[i + 1] : 0;

        uint8_t status    = (w0 >> 16) & 0xF0;
        uint8_t num_bytes = (w0 >> 16) & 0x0F;

        /* On START or COMPLETE: begin a new SysEx message */
        if(status == MIDI2_SYSEX7_START || status == MIDI2_SYSEX7_COMPLETE)
        {
            self->tx_len_ = 0;
            if(self->tx_len_ < kTxBufSize)
                self->tx_buf_[self->tx_len_++] = 0xF0;
        }

        /* Extract data bytes from UMP words */
        uint8_t bytes[6];
        bytes[0] = (w0 >> 8) & 0xFF;
        bytes[1] = w0 & 0xFF;
        bytes[2] = (w1 >> 24) & 0xFF;
        bytes[3] = (w1 >> 16) & 0xFF;
        bytes[4] = (w1 >> 8) & 0xFF;
        bytes[5] = w1 & 0xFF;

        for(uint8_t b = 0; b < num_bytes && b < 6; b++)
        {
            if(self->tx_len_ < kTxBufSize)
                self->tx_buf_[self->tx_len_++] = bytes[b];
        }

        /* On END or COMPLETE: append F7 and send */
        if(status == MIDI2_SYSEX7_END || status == MIDI2_SYSEX7_COMPLETE)
        {
            if(self->tx_len_ < kTxBufSize)
                self->tx_buf_[self->tx_len_++] = 0xF7;

            if(self->tx_cb_)
                self->tx_cb_(self->tx_buf_, self->tx_len_, self->tx_ctx_);

            self->tx_len_ = 0;
        }

        sent += (i + 1 < count) ? 2 : 1;
    }

    return sent;
}
