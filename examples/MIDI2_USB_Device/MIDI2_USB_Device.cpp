/** MIDI 2.0 USB Device -- Feature Showcase
 *
 *  Daisy Seed enumerates as "Daisy Seed MIDI 2.0" on USB.
 *  Sends various MIDI message types every 500ms to demonstrate
 *  the full midi2 pipeline:
 *
 *  - Note On / Note Off (C major arpeggio)
 *  - Control Change (modwheel sweep)
 *  - Pitch Bend (sweep up then reset)
 *  - Program Change (cycle through programs)
 *  - Channel Pressure (aftertouch sweep)
 *  - Poly Pressure (per-note)
 *
 *  Alt 0 (MIDI 1.0 host): sends CIN bytes
 *  Alt 1 (MIDI 2.0 host): sends UMP words with full resolution
 *
 *  Echoes back received NoteOn in both modes.
 *  LED toggles on each note.
 */
#include "daisy_seed.h"

extern "C"
{
    extern uint8_t usbd_midi2_alt_setting;
}

using namespace daisy;

DaisySeed       hw;
MidiUsbHandler  midi;
Midi2Processor  midi2;

static volatile uint8_t echo_note = 0;
static volatile uint8_t echo_vel  = 0;
static volatile bool    do_echo   = false;

void OnNoteOn(uint8_t  g,
              uint8_t  ch,
              uint8_t  note,
              uint16_t vel,
              uint8_t  at,
              uint16_t ad,
              void*    ctx)
{
    echo_note = note;
    echo_vel  = (uint8_t)(vel >> 9);
    do_echo   = true;
}

int main(void)
{
    hw.Init();

    /* USB MIDI 2.0 transport */
    MidiUsbHandler::Config midi_config;
    midi_config.transport_config.periph
        = MidiUsbTransport::Config::Periph::INTERNAL;
    midi.Init(midi_config);

    /* MIDI 2.0 processor */
    Midi2Processor::Config m2cfg;
    m2cfg.manufacturer_id = 0x7D;
    m2cfg.family_id       = 0x0001;
    m2cfg.model_id        = 0x0001;
    m2cfg.version_id      = 0x00010000;
    m2cfg.muid_seed       = 0xDA15;
    m2cfg.group           = 0;
    midi2.Init(m2cfg);

    midi2.GetDispatch()->upscale_mt2 = true;
    midi2.GetDispatch()->on_note_on  = OnNoteOn;

    midi.EnableMidi2(&midi2);
    midi.StartReceive();

    /* Demo state */
    const uint8_t arp_notes[] = {60, 64, 67, 72};
    uint8_t       arp_idx     = 0;
    uint8_t       prev_note   = 0;
    uint8_t       cc_val      = 0;
    int8_t        cc_dir      = 1;
    uint8_t       prog        = 0;
    uint8_t       pressure    = 0;
    int8_t        pres_dir    = 1;
    uint16_t      bend_val    = 0x2000; /* center */
    int16_t       bend_dir    = 0x100;
    uint8_t       step        = 0;
    bool          led         = false;
    uint32_t      last        = System::GetNow();

    while(1)
    {
        midi.Listen();
        while(midi.HasEvents())
            midi.PopEvent();

        /* Echo received notes */
        if(do_echo)
        {
            do_echo = false;
            if(usbd_midi2_alt_setting == 1)
            {
                uint32_t w[2];
                midi2_msg_note_on(w, 0, 0, echo_note,
                                  midi2_msg_scale_up_7to16(echo_vel), 0);
                midi.SendUmpMessage(w, 2);
            }
            else
            {
                uint8_t msg[3] = {0x90, echo_note, echo_vel};
                midi.SendMessage(msg, 3);
            }
        }

        /* Send demo messages every 500ms */
        uint32_t now = System::GetNow();
        if(now - last < 500)
        {
            System::Delay(1);
            continue;
        }
        last = now;

        if(usbd_midi2_alt_setting == 1)
        {
            /* ---- Alt 1: Native UMP (16/32-bit resolution) ---- */
            uint32_t w[2];

            switch(step)
            {
                case 0: /* Note On */
                    if(prev_note > 0)
                    {
                        midi2_msg_note_off(w, 0, 0, prev_note, 0x8000, 0);
                        midi.SendUmpMessage(w, 2);
                    }
                    midi2_msg_note_on(
                        w, 0, 0, arp_notes[arp_idx], 0xC000, 0);
                    midi.SendUmpMessage(w, 2);
                    prev_note = arp_notes[arp_idx];
                    arp_idx   = (arp_idx + 1) % 4;
                    break;

                case 1: /* CC: Modwheel (index 1) */
                    midi2_msg_cc(w, 0, 0, 1,
                                midi2_msg_scale_up_7to32(cc_val));
                    midi.SendUmpMessage(w, 2);
                    cc_val += cc_dir * 16;
                    if(cc_val >= 127)
                        cc_dir = -1;
                    if(cc_val == 0)
                        cc_dir = 1;
                    break;

                case 2: /* Pitch Bend */
                    midi2_msg_pitch_bend(
                        w, 0, 0,
                        midi2_msg_scale_up_14to32(bend_val));
                    midi.SendUmpMessage(w, 2);
                    bend_val += bend_dir;
                    if(bend_val >= 0x3FFF)
                        bend_dir = -0x100;
                    if(bend_val <= 0x0000)
                    {
                        bend_val = 0x2000;
                        bend_dir = 0x100;
                    }
                    break;

                case 3: /* Program Change */
                    midi2_msg_program(w, 0, 0, prog, false, 0, 0);
                    midi.SendUmpMessage(w, 2);
                    prog = (prog + 1) % 8;
                    break;

                case 4: /* Channel Pressure */
                    midi2_msg_chan_pressure(
                        w, 0, 0,
                        midi2_msg_scale_up_7to32(pressure));
                    midi.SendUmpMessage(w, 2);
                    pressure += pres_dir * 20;
                    if(pressure >= 127)
                        pres_dir = -1;
                    if(pressure == 0)
                        pres_dir = 1;
                    break;

                case 5: /* Poly Pressure on current note */
                    midi2_msg_poly_pressure(
                        w, 0, 0, prev_note,
                        midi2_msg_scale_up_7to32(80));
                    midi.SendUmpMessage(w, 2);
                    break;
            }
        }
        else
        {
            /* ---- Alt 0: MIDI 1.0 bytes (7-bit) ---- */
            uint8_t msg[3];

            switch(step)
            {
                case 0: /* Note On */
                    if(prev_note > 0)
                    {
                        msg[0] = 0x80;
                        msg[1] = prev_note;
                        msg[2] = 0x40;
                        midi.SendMessage(msg, 3);
                    }
                    msg[0]    = 0x90;
                    msg[1]    = arp_notes[arp_idx];
                    msg[2]    = 0x64;
                    midi.SendMessage(msg, 3);
                    prev_note = arp_notes[arp_idx];
                    arp_idx   = (arp_idx + 1) % 4;
                    break;

                case 1: /* CC: Modwheel */
                    msg[0] = 0xB0;
                    msg[1] = 0x01;
                    msg[2] = cc_val;
                    midi.SendMessage(msg, 3);
                    cc_val += cc_dir * 16;
                    if(cc_val >= 127)
                        cc_dir = -1;
                    if(cc_val == 0)
                        cc_dir = 1;
                    break;

                case 2: /* Pitch Bend */
                    msg[0] = 0xE0;
                    msg[1] = bend_val & 0x7F;
                    msg[2] = (bend_val >> 7) & 0x7F;
                    midi.SendMessage(msg, 3);
                    bend_val += bend_dir;
                    if(bend_val >= 0x3FFF)
                        bend_dir = -0x100;
                    if(bend_val <= 0x0000)
                    {
                        bend_val = 0x2000;
                        bend_dir = 0x100;
                    }
                    break;

                case 3: /* Program Change */
                    msg[0] = 0xC0;
                    msg[1] = prog;
                    midi.SendMessage(msg, 2);
                    prog = (prog + 1) % 8;
                    break;

                case 4: /* Channel Pressure */
                    msg[0] = 0xD0;
                    msg[1] = pressure;
                    midi.SendMessage(msg, 2);
                    pressure += pres_dir * 20;
                    if(pressure >= 127)
                        pres_dir = -1;
                    if(pressure == 0)
                        pres_dir = 1;
                    break;

                case 5: /* Poly Pressure */
                    msg[0] = 0xA0;
                    msg[1] = prev_note;
                    msg[2] = 80;
                    midi.SendMessage(msg, 3);
                    break;
            }
        }

        step = (step + 1) % 6;
        led  = !led;
        hw.SetLed(led);

        System::Delay(1);
    }
}
