/** MIDI 2.0 USB Device -- Feature Showcase
 *
 *  Daisy Seed enumerates as "Daisy Seed MIDI 2.0" on USB.
 *  Cycles through every UMP message family every 500ms.
 *
 *  Alt 0 (MIDI 1.0 host): sends CIN bytes for the 6 voice messages.
 *  Alt 1 (MIDI 2.0 host): sends UMP words and exposes the full
 *  MIDI 2.0 surface, including per-note expression, Flex Data,
 *  JR Timestamps and Stream Clip markers.
 *
 *  Echoes back received NoteOn in both modes.
 *  LED toggles on each step.
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

void OnNoteOn(uint8_t  /*group*/,
              uint8_t  /*channel*/,
              uint8_t  note,
              uint16_t velocity16,
              uint8_t  /*attr_type*/,
              uint16_t /*attr_data*/,
              void*    /*ctx*/)
{
    echo_note = note;
    echo_vel  = midi2_msg_scale_down_16to7(velocity16);
    do_echo   = true;
}

/*--------------------------------------------------------------------+
 * UMP demo cycle (Alt 1)
 *
 *  Voice messages (also reachable in MIDI 1.0, higher resolution):
 *    0  Note On + Note Off (16-bit velocity)
 *    1  Control Change (32-bit value)
 *    2  Pitch Bend (32-bit)
 *    3  Program Change
 *    4  Channel Pressure (32-bit)
 *    5  Poly Pressure (32-bit)
 *
 *  MIDI 2.0-only voice extensions:
 *    6  Note On + Pitch 7.9 attribute (microtonal)
 *    7  RPN (Pitch Bend Sensitivity)
 *    8  NRPN
 *    9  Relative RPN
 *   10  Relative NRPN
 *
 *  Per-Note expression (MIDI 2.0-only):
 *   11  Per-Note Pitch Bend
 *   12  Assignable Per-Note Controller
 *   13  Registered Per-Note Controller
 *   14  Per-Note Management (reset controllers)
 *
 *  Flex Data (MT 0xD, MIDI 2.0-only):
 *   15  Set Tempo (120 BPM)
 *   16  Time Signature (4/4)
 *   17  Key Signature (C major)
 *   18  Metronome
 *   19  Chord Name (C major)
 *   20  Flex Text (Project Name)
 *
 *  Utility + System Real-Time:
 *   21  JR Clock
 *   22  JR Timestamp
 *   23  DCTPQ + Delta Clockstamp
 *   24  Song Position
 *   25  Timing Clock burst
 *
 *  Stream Clip markers (MT 0xF):
 *   26  Start of Clip
 *   27  End of Clip
 *
 *  SysEx7:
 *   28  SysEx7 complete packet
 *--------------------------------------------------------------------*/
static constexpr uint8_t kDemoSteps = 29;
static constexpr uint8_t kAlt0Steps = 6;

int main(void)
{
    hw.Init();

    MidiUsbHandler::Config midi_config;
    midi_config.transport_config.periph
        = MidiUsbTransport::Config::Periph::INTERNAL;
    midi.Init(midi_config);

    /* MIDI 2.0 processor. Defaults: nak_on_unknown = true. */
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
    uint16_t      bend_val    = 0x2000;
    int16_t       bend_dir    = 0x100;
    uint16_t      pitch79     = 0;
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
                                  midi2_msg_scale_up_7to16(echo_vel),
                                  0, 0);
                midi.SendUmpMessage(w, 2);
            }
            else
            {
                uint8_t msg[3] = {0x90, echo_note, echo_vel};
                midi.SendMessage(msg, 3);
            }
        }

        uint32_t now = System::GetNow();
        if(now - last < 500)
        {
            System::Delay(1);
            continue;
        }
        last = now;

        if(usbd_midi2_alt_setting == 1)
        {
            /* ---- Alt 1: Native UMP ---- */
            uint32_t w[4];

            switch(step)
            {
                /*------ Voice messages (high-res) ------*/
                case 0: /* Note On + Note Off */
                    if(prev_note > 0)
                    {
                        midi2_msg_note_off(w, 0, 0, prev_note, 0x8000, 0, 0);
                        midi.SendUmpMessage(w, 2);
                    }
                    midi2_msg_note_on(w, 0, 0, arp_notes[arp_idx],
                                      0xC000, 0, 0);
                    midi.SendUmpMessage(w, 2);
                    prev_note = arp_notes[arp_idx];
                    arp_idx   = (arp_idx + 1) % 4;
                    break;

                case 1: /* CC: Modwheel */
                    midi2_msg_cc(w, 0, 0, 1,
                                 midi2_msg_scale_up_7to32(cc_val));
                    midi.SendUmpMessage(w, 2);
                    cc_val += cc_dir * 16;
                    if(cc_val >= 127) cc_dir = -1;
                    if(cc_val == 0)   cc_dir = 1;
                    break;

                case 2: /* Pitch Bend */
                    midi2_msg_pitch_bend(w, 0, 0,
                        midi2_msg_scale_up_14to32(bend_val));
                    midi.SendUmpMessage(w, 2);
                    bend_val += bend_dir;
                    if(bend_val >= 0x3FFF) bend_dir = -0x100;
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
                    midi2_msg_chan_pressure(w, 0, 0,
                        midi2_msg_scale_up_7to32(pressure));
                    midi.SendUmpMessage(w, 2);
                    pressure += pres_dir * 20;
                    if(pressure >= 127) pres_dir = -1;
                    if(pressure == 0)   pres_dir = 1;
                    break;

                case 5: /* Poly Pressure */
                    midi2_msg_poly_pressure(w, 0, 0, prev_note,
                        midi2_msg_scale_up_7to32(80));
                    midi.SendUmpMessage(w, 2);
                    break;

                /*------ MIDI 2.0-only voice extensions ------*/
                case 6: /* Note On + Pitch 7.9 microtonal attribute */
                {
                    /* attr_data: 7-bit semitone | 9-bit fractional cent */
                    uint16_t frac = (pitch79 + 64) & 0x1FF;
                    pitch79 = frac;
                    uint16_t attr = (uint16_t)((64u << 9) | frac);
                    midi2_msg_note_on(w, 0, 0, 64, 0xC000,
                                      /*attr_type Pitch7_9*/ 0x03, attr);
                    midi.SendUmpMessage(w, 2);
                    break;
                }

                case 7: /* RPN: Pitch Bend Sensitivity (msb=0, lsb=0) */
                    midi2_msg_rpn(w, 0, 0, 0, 0, 0x02000000);
                    midi.SendUmpMessage(w, 2);
                    break;

                case 8: /* NRPN (manufacturer-specific) */
                    midi2_msg_nrpn(w, 0, 0, 0x10, 0x20, 0x40000000);
                    midi.SendUmpMessage(w, 2);
                    break;

                case 9: /* Relative RPN */
                    midi2_msg_rel_rpn(w, 0, 0, 0, 0, 1);
                    midi.SendUmpMessage(w, 2);
                    break;

                case 10: /* Relative NRPN */
                    midi2_msg_rel_nrpn(w, 0, 0, 0x10, 0x20, 0xFFFFFFFFu);
                    midi.SendUmpMessage(w, 2);
                    break;

                /*------ Per-Note expression ------*/
                case 11: /* Per-Note Pitch Bend (independent per voice) */
                    midi2_msg_per_note_pb(w, 0, 0, prev_note, 0x90000000u);
                    midi.SendUmpMessage(w, 2);
                    break;

                case 12: /* Assignable Per-Note Controller (index 74, brightness) */
                    midi2_msg_asn_per_note_ctrl(w, 0, 0, prev_note, 74,
                                                0xC0000000u);
                    midi.SendUmpMessage(w, 2);
                    break;

                case 13: /* Registered Per-Note Controller (index 1, Modulation) */
                    midi2_msg_reg_per_note_ctrl(w, 0, 0, prev_note, 1,
                                                0x80000000u);
                    midi.SendUmpMessage(w, 2);
                    break;

                case 14: /* Per-Note Management: reset controllers */
                    midi2_msg_per_note_mgmt(w, 0, 0, prev_note,
                                            /*detach*/ false,
                                            /*reset*/  true);
                    midi.SendUmpMessage(w, 2);
                    break;

                /*------ Flex Data (MT 0xD, 4-word) ------*/
                case 15: /* Set Tempo: 120 BPM = 500000 us/QN = 50000000 ten-ns */
                    midi2_msg_tempo(w, 0, 50000000u);
                    midi.SendUmpMessage(w, 4);
                    break;

                case 16: /* Time Signature 4/4, 8 32nd notes */
                    midi2_msg_time_sig(w, 0, 4, 2, 8);
                    midi.SendUmpMessage(w, 4);
                    break;

                case 17: /* Key Signature: C major (0 sharps, major) */
                    midi2_msg_key_sig(w, 0, 0, false);
                    midi.SendUmpMessage(w, 4);
                    break;

                case 18: /* Metronome: 4 clicks/bar */
                    midi2_msg_metronome(w, 0, 4, 1, 0, 0, 0, 0);
                    midi.SendUmpMessage(w, 4);
                    break;

                case 19: /* Chord Name: C major */
                    midi2_msg_chord_name(w, 0,
                        /*address group*/ 1, 0,
                        /*tonic*/ 0, /*C=3*/ 3, /*type major*/ 1,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0);
                    midi.SendUmpMessage(w, 4);
                    break;

                case 20: /* Flex Text: Project Name "DaisyMIDI2" */
                {
                    const uint8_t text[] = {'D','a','i','s','y','M','I','D','I','2'};
                    midi2_msg_flex_text(w, 0,
                        /*format complete*/ 0,
                        /*address channel*/ 0, 0,
                        /*bank metadata*/   0x01,
                        /*status project*/  0x01,
                        text, sizeof(text));
                    midi.SendUmpMessage(w, 4);
                    break;
                }

                /*------ Utility + System Real-Time ------*/
                case 21: /* JR Clock */
                {
                    uint32_t word = midi2_msg_jr_clock((uint16_t)(now & 0xFFFF));
                    midi.SendUmpMessage(&word, 1);
                    break;
                }

                case 22: /* JR Timestamp */
                {
                    uint32_t word = midi2_msg_jr_timestamp(2000);
                    midi.SendUmpMessage(&word, 1);
                    break;
                }

                case 23: /* DCTPQ + Delta Clockstamp */
                {
                    uint32_t word = midi2_msg_dctpq(960);
                    midi.SendUmpMessage(&word, 1);
                    word = midi2_msg_delta_clockstamp(240);
                    midi.SendUmpMessage(&word, 1);
                    break;
                }

                case 24: /* Song Position */
                {
                    uint32_t word = midi2_msg_system_song_position(0, 16);
                    midi.SendUmpMessage(&word, 1);
                    break;
                }

                case 25: /* Timing Clock burst (8 ticks) */
                    for(uint8_t i = 0; i < 8; i++)
                    {
                        uint32_t word = midi2_msg_system_timing_clock(0);
                        midi.SendUmpMessage(&word, 1);
                    }
                    break;

                /*------ Stream Clip markers ------*/
                case 26: /* Start of Clip */
                    midi2_msg_stream_start_of_clip(w);
                    midi.SendUmpMessage(w, 4);
                    break;

                case 27: /* End of Clip */
                    midi2_msg_stream_end_of_clip(w);
                    midi.SendUmpMessage(w, 4);
                    break;

                /*------ SysEx7 ------*/
                case 28: /* SysEx7 complete: 7D 01 02 03 */
                {
                    const uint8_t payload[] = {0x7D, 0x01, 0x02, 0x03};
                    midi2_msg_sysex7_packet(w, 0,
                        /*COMPLETE*/ 0x00, payload, sizeof(payload));
                    midi.SendUmpMessage(w, 2);
                    break;
                }
            }

            step = (step + 1) % kDemoSteps;
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
                    msg[0] = 0x90;
                    msg[1] = arp_notes[arp_idx];
                    msg[2] = 0x64;
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
                    if(cc_val >= 127) cc_dir = -1;
                    if(cc_val == 0)   cc_dir = 1;
                    break;

                case 2: /* Pitch Bend */
                    msg[0] = 0xE0;
                    msg[1] = bend_val & 0x7F;
                    msg[2] = (bend_val >> 7) & 0x7F;
                    midi.SendMessage(msg, 3);
                    bend_val += bend_dir;
                    if(bend_val >= 0x3FFF) bend_dir = -0x100;
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
                    if(pressure >= 127) pres_dir = -1;
                    if(pressure == 0)   pres_dir = 1;
                    break;

                case 5: /* Poly Pressure */
                    msg[0] = 0xA0;
                    msg[1] = prev_note;
                    msg[2] = 80;
                    midi.SendMessage(msg, 3);
                    break;
            }

            step = (step + 1) % kAlt0Steps;
        }

        led = !led;
        hw.SetLed(led);

        System::Delay(1);
    }
}
