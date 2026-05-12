/** MIDI 2.0 USB Host with SSD1306 OLED Monitor
 *
 *  Daisy Seed acts as USB Host. When a MIDI device is plugged in,
 *  the host sends a MIDI-CI Discovery Request. Incoming UMP traffic
 *  is decoded by Midi2Processor and printed live to a 128x64 SSD1306
 *  OLED connected via I2C1 (SCL=D11, SDA=D12, 0x3C).
 *
 *  The display keeps a fixed header ("Daisyseed MIDI 2.0") and a
 *  7-line scroll buffer that captures every MIDI 2.0 message family
 *  the upstream device emits: voice (high-res), per-note expression,
 *  RPN/NRPN, Flex Data, JR Timestamps, Stream Clip markers.
 *
 *  Requires USB-A connector on Daisy Seed.
 */
#include "daisy_seed.h"
#include "dev/oled_ssd130x.h"
#include "hid/disp/oled_display.h"
#include "usbh_midi.h"

#include <cstdio>
#include <cstring>

using namespace daisy;

/*--------------------------------------------------------------------+
 * Hardware
 *--------------------------------------------------------------------*/
using MyOledDisplay = OledDisplay<SSD130xI2c128x64Driver>;

DaisySeed       hw;
MidiUsbHandler  midi;
Midi2Processor  midi2;
USBHostHandle   usbHost;
MyOledDisplay   display;

/*--------------------------------------------------------------------+
 * Display state
 *
 *  128x64 with Font_6x8 = 8 rows of 21 chars.
 *  Row 0   : header "Daisyseed MIDI 2.0"
 *  Rows 1-7: scroll buffer (newest at bottom)
 *--------------------------------------------------------------------*/
static constexpr uint8_t kCols       = 21;
static constexpr uint8_t kScrollRows = 7;
static char              scroll_buf[kScrollRows][kCols + 1];
static volatile bool     display_dirty = true;

static void PushLine(const char* line)
{
    for(uint8_t i = 0; i < kScrollRows - 1; i++)
    {
        std::memcpy(scroll_buf[i], scroll_buf[i + 1], kCols + 1);
    }
    std::strncpy(scroll_buf[kScrollRows - 1], line, kCols);
    scroll_buf[kScrollRows - 1][kCols] = '\0';
    display_dirty = true;
}

static void DrawDisplay()
{
    display.Fill(false);
    display.SetCursor(0, 0);
    display.WriteString("Daisyseed MIDI 2.0", Font_6x8, true);
    for(uint8_t i = 0; i < kScrollRows; i++)
    {
        display.SetCursor(0, (uint16_t)((i + 1) * 8));
        display.WriteString(scroll_buf[i], Font_6x8, true);
    }
    display.Update();
}

static void ClearScroll()
{
    for(uint8_t i = 0; i < kScrollRows; i++)
        scroll_buf[i][0] = '\0';
    display_dirty = true;
}

/*--------------------------------------------------------------------+
 * State
 *--------------------------------------------------------------------*/
static volatile bool device_connected = false;
static volatile bool ci_discovery_sent = false;

/*--------------------------------------------------------------------+
 * UMP dispatch callbacks -> push formatted line to scroll buffer
 *--------------------------------------------------------------------*/
static void OnNoteOn(uint8_t group, uint8_t channel, uint8_t note,
                     uint16_t velocity, uint8_t /*attr_type*/,
                     uint16_t /*attr_data*/, void* /*ctx*/)
{
    (void)group;
    hw.SetLed(true);
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "NOn  ch%-2u n%-3u v%05u",
                  channel, note, velocity);
    PushLine(line);
}

static void OnNoteOff(uint8_t group, uint8_t channel, uint8_t note,
                      uint16_t velocity, uint8_t /*attr_type*/,
                      uint16_t /*attr_data*/, void* /*ctx*/)
{
    (void)group;
    hw.SetLed(false);
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "NOff ch%-2u n%-3u v%05u",
                  channel, note, velocity);
    PushLine(line);
}

static void OnCC(uint8_t group, uint8_t channel, uint8_t index,
                 uint32_t value, void* /*ctx*/)
{
    (void)group;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "CC   ch%-2u i%-3u %08lX",
                  channel, index, (unsigned long)value);
    PushLine(line);
}

static void OnProgram(uint8_t group, uint8_t channel, uint8_t program,
                      bool bank_valid, uint8_t /*bank_msb*/,
                      uint8_t /*bank_lsb*/, void* /*ctx*/)
{
    (void)group;
    (void)bank_valid;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PCh  ch%-2u p%-3u",
                  channel, program);
    PushLine(line);
}

static void OnChanPressure(uint8_t group, uint8_t channel,
                           uint32_t value, void* /*ctx*/)
{
    (void)group;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "ChP  ch%-2u %08lX",
                  channel, (unsigned long)value);
    PushLine(line);
}

static void OnPolyPressure(uint8_t group, uint8_t channel, uint8_t note,
                           uint32_t value, void* /*ctx*/)
{
    (void)group;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PPr  ch%-2u n%-3u %08lX",
                  channel, note, (unsigned long)value);
    PushLine(line);
}

static void OnPitchBend(uint8_t group, uint8_t channel,
                        uint32_t value, void* /*ctx*/)
{
    (void)group;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PB   ch%-2u %08lX",
                  channel, (unsigned long)value);
    PushLine(line);
}

static void OnPerNotePB(uint8_t group, uint8_t channel, uint8_t note,
                        uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PNPB n%-3u %08lX",
                  note, (unsigned long)value);
    PushLine(line);
}

static void OnAsnPerNote(uint8_t group, uint8_t channel, uint8_t note,
                         uint8_t index, uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PNCa n%-3u i%-3u %04lX",
                  note, index, (unsigned long)(value >> 16));
    PushLine(line);
}

static void OnRegPerNote(uint8_t group, uint8_t channel, uint8_t note,
                         uint8_t index, uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PNCr n%-3u i%-3u %04lX",
                  note, index, (unsigned long)(value >> 16));
    PushLine(line);
}

static void OnPerNoteMgmt(uint8_t group, uint8_t channel, uint8_t note,
                          bool detach, bool reset, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "PNMg n%-3u D%u R%u",
                  note, detach ? 1u : 0u, reset ? 1u : 0u);
    PushLine(line);
}

static void OnRpn(uint8_t group, uint8_t channel, uint8_t msb, uint8_t lsb,
                  uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "RPN  m%-3u l%-3u %04lX",
                  msb, lsb, (unsigned long)(value >> 16));
    PushLine(line);
}

static void OnNrpn(uint8_t group, uint8_t channel, uint8_t msb, uint8_t lsb,
                   uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "NRPN m%-3u l%-3u %04lX",
                  msb, lsb, (unsigned long)(value >> 16));
    PushLine(line);
}

static void OnRelRpn(uint8_t group, uint8_t channel, uint8_t msb, uint8_t lsb,
                     uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "RRPN m%-3u l%-3u %04lX",
                  msb, lsb, (unsigned long)(value >> 16));
    PushLine(line);
}

static void OnRelNrpn(uint8_t group, uint8_t channel, uint8_t msb, uint8_t lsb,
                      uint32_t value, void* /*ctx*/)
{
    (void)group;
    (void)channel;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line),
                  "RNRP m%-3u l%-3u %04lX",
                  msb, lsb, (unsigned long)(value >> 16));
    PushLine(line);
}

/*--------------------------------------------------------------------+
 * Flex Data
 *--------------------------------------------------------------------*/
static void OnTempo(uint8_t group, uint32_t ten_ns_per_qn, void* /*ctx*/)
{
    (void)group;
    /* BPM = 6e9 / (ten_ns_per_qn) */
    uint32_t bpm = (ten_ns_per_qn > 0)
                       ? (uint32_t)(6000000000ULL / ten_ns_per_qn)
                       : 0;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "Tempo %lu BPM",
                  (unsigned long)bpm);
    PushLine(line);
}

static void OnTimeSig(uint8_t group, uint8_t numerator, uint8_t denominator,
                      uint8_t num_32nd_notes, void* /*ctx*/)
{
    (void)group;
    (void)num_32nd_notes;
    uint8_t denom_pow2 = (denominator == 0) ? 0 : (uint8_t)(1u << denominator);
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "TimSig %u/%u",
                  numerator, denom_pow2);
    PushLine(line);
}

static void OnKeySig(uint8_t group, uint8_t address, uint8_t channel,
                     int8_t sharps_flats, uint8_t tonic_note,
                     uint8_t key_type, void* /*ctx*/)
{
    (void)group;
    (void)address;
    (void)channel;
    (void)tonic_note;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "KeySig sf=%-3d %s",
                  (int)sharps_flats,
                  key_type == 1 ? "min" : "maj");
    PushLine(line);
}

static void OnMetronome(uint8_t group, uint8_t primary_clicks,
                        uint8_t accent_1, uint8_t /*accent_2*/,
                        uint8_t /*accent_3*/, uint8_t /*subdiv_1*/,
                        uint8_t /*subdiv_2*/, void* /*ctx*/)
{
    (void)group;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "Metronome %u/%u",
                  primary_clicks, accent_1);
    PushLine(line);
}

static const char* TonicName(uint8_t t)
{
    static const char* names[8] = {"?", "C", "D", "E", "F", "G", "A", "B"};
    return (t < 8) ? names[t] : "?";
}

static void OnChord(uint8_t group, uint8_t address, uint8_t channel,
                    int8_t tonic_sf, uint8_t tonic_note, uint8_t chord_type,
                    uint8_t /*alt1_type*/, uint8_t /*alt1_deg*/,
                    uint8_t /*alt2_type*/, uint8_t /*alt2_deg*/,
                    uint8_t /*alt3_type*/, uint8_t /*alt3_deg*/,
                    uint8_t /*alt4_type*/, uint8_t /*alt4_deg*/,
                    int8_t /*bass_sf*/, uint8_t /*bass_note*/,
                    uint8_t /*bass_type*/,
                    uint8_t /*bass_alt1_type*/, uint8_t /*bass_alt1_deg*/,
                    uint8_t /*bass_alt2_type*/, uint8_t /*bass_alt2_deg*/,
                    void* /*ctx*/)
{
    (void)group;
    (void)address;
    (void)channel;
    char line[kCols + 1];
    char sf = (tonic_sf > 0) ? '#' : (tonic_sf < 0 ? 'b' : ' ');
    std::snprintf(line, sizeof(line), "Chord %s%c t=%u",
                  TonicName(tonic_note), sf, chord_type);
    PushLine(line);
}

static void OnFlexText(uint8_t group, uint8_t format, uint8_t address,
                       uint8_t channel, uint8_t bank, uint8_t status,
                       const uint8_t* text, uint8_t len, void* /*ctx*/)
{
    (void)group;
    (void)format;
    (void)address;
    (void)channel;
    (void)bank;
    (void)status;
    char line[kCols + 1];
    /* Reserve 6 chars for prefix "FlxTx " => 15 chars payload */
    uint8_t copy = len > 15 ? 15 : len;
    char    payload[16];
    for(uint8_t i = 0; i < copy; i++)
        payload[i] = (text[i] >= 0x20 && text[i] < 0x7F) ? (char)text[i] : '.';
    payload[copy] = '\0';
    std::snprintf(line, sizeof(line), "FlxTx %s", payload);
    PushLine(line);
}

/*--------------------------------------------------------------------+
 * Utility / System / Stream
 *--------------------------------------------------------------------*/
static void OnJrClock(uint16_t timestamp, void* /*ctx*/)
{
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "JR   t=%u", timestamp);
    PushLine(line);
}

static void OnJrTimestamp(uint16_t timestamp, void* /*ctx*/)
{
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "JRT  t=%u", timestamp);
    PushLine(line);
}

static void OnDctpq(uint16_t tpq, void* /*ctx*/)
{
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "DCTPQ %u", tpq);
    PushLine(line);
}

static void OnDc(uint32_t ticks, void* /*ctx*/)
{
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "DC   %lu",
                  (unsigned long)ticks);
    PushLine(line);
}

static void OnSystem(uint8_t group, uint8_t status, uint8_t data1,
                     uint8_t data2, void* /*ctx*/)
{
    (void)group;
    (void)data1;
    (void)data2;
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "Sys  st=0x%02X", status);
    PushLine(line);
}

static void OnClip(bool is_start, void* /*ctx*/)
{
    PushLine(is_start ? "Clip Start" : "Clip End");
}

/*--------------------------------------------------------------------+
 * Send CI Discovery Request to the connected device
 *--------------------------------------------------------------------*/
static void SendCIDiscovery()
{
    midi2_ci_state* ci = midi2.GetCI();

    uint8_t  discovery[32];
    uint16_t len = midi2_ci_build_discovery(
        discovery,
        MIDI2_CI_VERSION_1,
        ci->muid,
        ci->manufacturer_id,
        ci->family_id,
        ci->model_id,
        ci->version_id,
        MIDI2_CI_CAT_PROFILE_CONFIG
            | MIDI2_CI_CAT_PROPERTY_EXCHANGE
            | MIDI2_CI_CAT_PROCESS_INQUIRY,
        512,
        0);

    uint8_t sysex[64];
    sysex[0] = 0xF0;
    for(uint16_t i = 0; i < len && i < 62; i++)
        sysex[i + 1] = discovery[i];
    sysex[len + 1] = 0xF7;

    midi.SendMessage(sysex, len + 2);

    PushLine("CI Discovery sent");
    ci_discovery_sent = true;
}

/*--------------------------------------------------------------------+
 * USB Host callbacks
 *--------------------------------------------------------------------*/
static void USBH_Connect(void* /*data*/)
{
    PushLine("USB connected");
    device_connected = true;
}

static void USBH_Disconnect(void* /*data*/)
{
    PushLine("USB disconnected");
    device_connected  = false;
    ci_discovery_sent = false;
}

static void USBH_ClassActive(void* /*data*/)
{
    if(usbHost.IsActiveClass(USBH_MIDI_CLASS))
    {
        PushLine("MIDI class active");

        MidiUsbHandler::Config midi_config;
        midi_config.transport_config.periph
            = MidiUsbTransport::Config::Periph::HOST;
        midi.Init(midi_config);
        midi.EnableMidi2(&midi2);
        midi.StartReceive();

        System::Delay(100);
        SendCIDiscovery();
    }
}

static void USBH_Error(void* /*data*/)
{
    PushLine("USB error");
}

/*--------------------------------------------------------------------+
 * Splash screen
 *--------------------------------------------------------------------*/
static void ShowSplash()
{
    display.Fill(false);

    display.SetCursor(0, 0);
    display.WriteString("Daisyseed", Font_7x10, true);
    display.SetCursor(0, 12);
    display.WriteString("MIDI 2.0 Host", Font_7x10, true);

    display.SetCursor(0, 32);
    display.WriteString("midi2 v0.4.0", Font_6x8, true);
    display.SetCursor(0, 42);
    display.WriteString("libDaisy v8.1.0", Font_6x8, true);
    display.SetCursor(0, 54);
    display.WriteString("Waiting USB...", Font_6x8, true);

    display.Update();
}

/*--------------------------------------------------------------------+
 * Main
 *--------------------------------------------------------------------*/
int main(void)
{
    hw.Init();

    /* ---- OLED ---- */
    MyOledDisplay::Config disp_cfg; /* defaults: I2C1 PB8/PB9, 0x3C */
    display.Init(disp_cfg);
    ClearScroll();
    ShowSplash();

    /* ---- MIDI 2.0 processor ---- */
    Midi2Processor::Config m2cfg;
    m2cfg.manufacturer_id = 0x7D;
    m2cfg.family_id       = 0x0001;
    m2cfg.model_id        = 0x0001;
    m2cfg.version_id      = 0x00010000;
    m2cfg.muid_seed       = 0xDA15;
    m2cfg.group           = 0;
    midi2.Init(m2cfg);

    /* Unified MT 0x4 callbacks (upscale MIDI 1.0 to 2.0) */
    midi2_dispatch* dp = midi2.GetDispatch();
    dp->upscale_mt2 = true;

    /* Voice */
    dp->on_note_on       = OnNoteOn;
    dp->on_note_off      = OnNoteOff;
    dp->on_cc            = OnCC;
    dp->on_program       = OnProgram;
    dp->on_chan_pressure = OnChanPressure;
    dp->on_poly_pressure = OnPolyPressure;
    dp->on_pitch_bend    = OnPitchBend;

    /* Per-Note (MIDI 2.0 only) */
    dp->on_per_note_pb   = OnPerNotePB;
    dp->on_asn_per_note  = OnAsnPerNote;
    dp->on_reg_per_note  = OnRegPerNote;
    dp->on_per_note_mgmt = OnPerNoteMgmt;

    /* RPN / NRPN */
    dp->on_rpn      = OnRpn;
    dp->on_nrpn     = OnNrpn;
    dp->on_rel_rpn  = OnRelRpn;
    dp->on_rel_nrpn = OnRelNrpn;

    /* Flex Data */
    dp->on_tempo     = OnTempo;
    dp->on_time_sig  = OnTimeSig;
    dp->on_key_sig   = OnKeySig;
    dp->on_metronome = OnMetronome;
    dp->on_chord     = OnChord;
    dp->on_flex_text = OnFlexText;

    /* Utility / System / Stream */
    dp->on_jr_clock     = OnJrClock;
    dp->on_jr_timestamp = OnJrTimestamp;
    dp->on_dctpq        = OnDctpq;
    dp->on_dc           = OnDc;
    dp->on_system       = OnSystem;
    dp->on_clip         = OnClip;

    /* Show splash for ~2 s before bringing the USB host up. We hold
     * off on usbHost.Init() so the very first connect event is not
     * missed while the OLED is still doing its intro animation. */
    uint32_t splash_until = System::GetNow() + 2000;
    while(System::GetNow() < splash_until)
    {
        System::Delay(10);
    }
    ClearScroll();
    PushLine("Host ready");
    DrawDisplay();
    display_dirty = false;

    /* ---- USB Host (init AFTER the splash) ---- */
    USBHostHandle::Config usbh_config;
    usbh_config.connect_callback      = USBH_Connect;
    usbh_config.disconnect_callback   = USBH_Disconnect;
    usbh_config.class_active_callback = USBH_ClassActive;
    usbh_config.error_callback        = USBH_Error;
    usbHost.Init(usbh_config);
    usbHost.RegisterClass(USBH_MIDI_CLASS);

    /* ---- Main loop ---- */
    uint32_t last_draw = 0;
    while(1)
    {
        usbHost.Process();

        if(usbHost.IsActiveClass(USBH_MIDI_CLASS) && midi.RxActive())
        {
            midi.Listen();
            while(midi.HasEvents())
                midi.PopEvent();
        }

        /* Throttle display refresh to ~30 fps */
        uint32_t now = System::GetNow();
        if(display_dirty && (now - last_draw) >= 33)
        {
            display_dirty = false;
            last_draw     = now;
            DrawDisplay();
        }
    }
}
