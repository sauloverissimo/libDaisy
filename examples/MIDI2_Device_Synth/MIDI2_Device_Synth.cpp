/** Example: MIDI 2.0 Device (Synth) via UART
 *
 *  Demonstrates the unified MIDI 1.0 + MIDI 2.0 architecture.
 *
 *  With upscale_mt2 enabled, the dispatch translates MIDI 1.0 (MT 0x2)
 *  to MIDI 2.0 (MT 0x4) automatically. The application registers a
 *  single set of callbacks and receives both protocols with full
 *  resolution -- no protocol-specific handling needed.
 *
 *  MIDI-CI Discovery, Profile Inquiry, and PE are handled automatically
 *  by the Midi2Processor.
 *
 *  Any 5-pin DIN or TRS connector wired to UART Rx/Tx on Daisy Seed.
 *  Default: D14 (USART1 Rx), D13 (USART1 Tx).
 */
#include "daisy_seed.h"

using namespace daisy;

/*--------------------------------------------------------------------+
 * Hardware and MIDI
 *--------------------------------------------------------------------*/
DaisySeed       hw;
MidiUartHandler midi;
Midi2Processor  midi2;

/*--------------------------------------------------------------------+
 * Simple synth state
 *--------------------------------------------------------------------*/
struct SynthVoice
{
    uint8_t  note;
    uint16_t velocity;
    bool     active;
};

static SynthVoice voice = {0, 0, false};

/*--------------------------------------------------------------------+
 * Callbacks (MT 0x4 -- receives both MIDI 1.0 and 2.0 via upscale)
 *--------------------------------------------------------------------*/
void OnNoteOn(uint8_t  group,
              uint8_t  channel,
              uint8_t  note,
              uint16_t velocity,
              uint8_t  attr_type,
              uint16_t attr_data,
              void*    context)
{
    voice.note     = note;
    voice.velocity = velocity;
    voice.active   = true;
    hw.SetLed(true);
}

void OnNoteOff(uint8_t  group,
               uint8_t  channel,
               uint8_t  note,
               uint16_t velocity,
               uint8_t  attr_type,
               uint16_t attr_data,
               void*    context)
{
    if(voice.note == note)
    {
        voice.active = false;
        hw.SetLed(false);
    }
}

void OnCC(uint8_t  group,
          uint8_t  channel,
          uint8_t  index,
          uint32_t value,
          void*    context)
{
    (void)group;
    (void)channel;
    (void)index;
    (void)value;
}

/*--------------------------------------------------------------------+
 * Main
 *--------------------------------------------------------------------*/
int main(void)
{
    hw.Init();
    hw.StartLog();

    /* ---- Transport ---- */
    MidiUartHandler::Config midi_config;
    midi.Init(midi_config);

    /* ---- MIDI 2.0 processor ---- */
    Midi2Processor::Config m2cfg;
    m2cfg.manufacturer_id = 0x7D;       /* prototyping */
    m2cfg.family_id       = 0x0001;
    m2cfg.model_id        = 0x0001;
    m2cfg.version_id      = 0x00010000; /* v1.0 */
    m2cfg.muid_seed       = 0xDAE5;     /* unique per device */
    m2cfg.group           = 0;
    midi2.Init(m2cfg);

    /* Upscale: MIDI 1.0 bytes -> MT 0x2 -> MT 0x4 -> same callbacks */
    midi2.GetDispatch()->upscale_mt2 = true;
    midi2.GetDispatch()->on_note_on  = OnNoteOn;
    midi2.GetDispatch()->on_note_off = OnNoteOff;
    midi2.GetDispatch()->on_cc       = OnCC;

    /* Connect to handler and start */
    midi.EnableMidi2(&midi2);
    midi.StartReceive();

    hw.PrintLine("MIDI 2.0 Device ready");

    /* ---- Main loop ---- */
    while(1)
    {
        midi.Listen();

        /* Callbacks fire inline from Parse().
         * HasEvents()/PopEvent() still works for MIDI 1.0 if needed. */

        System::Delay(1);
    }
}
