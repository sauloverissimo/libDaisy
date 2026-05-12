/** Example: MIDI 2.0 Host via USB
 *
 *  The Daisy acts as USB Host. When a MIDI device is plugged in,
 *  the host sends a MIDI-CI Discovery Request. If the device replies,
 *  MIDI 2.0 capabilities are confirmed and the upscale dispatch is
 *  ready for high-resolution data.
 *
 *  If the device does not reply (MIDI 1.0 only), the upscale dispatch
 *  still works -- it translates MT 0x2 to MT 0x4 with proper scaling.
 *  The application code is the same either way.
 *
 *  Requires USB-A connector on Daisy Seed.
 */
#include "daisy_seed.h"
#include "usbh_midi.h"

/* midi2 C99 surface (CI message builders, dispatch, proc) comes in via
 * daisy_seed.h -> hid/midi2_processor.h -> hid/midi2/midi2.h (amalgam). */

using namespace daisy;

/*--------------------------------------------------------------------+
 * Hardware and MIDI
 *--------------------------------------------------------------------*/
DaisySeed       hw;
MidiUsbHandler  midi;
Midi2Processor  midi2;
USBHostHandle   usbHost;

/*--------------------------------------------------------------------+
 * State
 *--------------------------------------------------------------------*/
static bool device_connected = false;
static bool ci_discovery_sent = false;

/*--------------------------------------------------------------------+
 * Callbacks (MT 0x4 -- works for MIDI 1.0 and 2.0 via upscale)
 *--------------------------------------------------------------------*/
void OnNoteOn(uint8_t  group,
              uint8_t  channel,
              uint8_t  note,
              uint16_t velocity,
              uint8_t  attr_type,
              uint16_t attr_data,
              void*    context)
{
    hw.SetLed(true);

    char buf[64];
    sprintf(buf, "NoteOn: ch=%d note=%d vel=%u", channel, note, velocity);
    hw.PrintLine(buf);
}

void OnNoteOff(uint8_t  group,
               uint8_t  channel,
               uint8_t  note,
               uint16_t velocity,
               uint8_t  attr_type,
               uint16_t attr_data,
               void*    context)
{
    hw.SetLed(false);
}

void OnCC(uint8_t  group,
          uint8_t  channel,
          uint8_t  index,
          uint32_t value,
          void*    context)
{
    char buf[64];
    sprintf(buf, "CC: ch=%d idx=%d val=%lu", channel, index, (unsigned long)value);
    hw.PrintLine(buf);
}

/*--------------------------------------------------------------------+
 * Send CI Discovery Request to the connected device
 *--------------------------------------------------------------------*/
void SendCIDiscovery()
{
    midi2_ci_state* ci = midi2.GetCI();

    /* Build Discovery Request SysEx (without F0/F7) */
    uint8_t discovery[32];
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
        512,    /* max sysex size */
        0);     /* output path ID */

    /* Wrap in F0/F7 and send */
    uint8_t sysex[64];
    sysex[0] = 0xF0;
    for(uint16_t i = 0; i < len && i < 62; i++)
        sysex[i + 1] = discovery[i];
    sysex[len + 1] = 0xF7;

    midi.SendMessage(sysex, len + 2);

    hw.PrintLine("CI Discovery sent");
    ci_discovery_sent = true;
}

/*--------------------------------------------------------------------+
 * USB Host callbacks
 *--------------------------------------------------------------------*/
void USBH_Connect(void* data)
{
    hw.PrintLine("USB device connected");
    device_connected = true;
}

void USBH_Disconnect(void* data)
{
    hw.PrintLine("USB device disconnected");
    device_connected  = false;
    ci_discovery_sent = false;
}

void USBH_ClassActive(void* data)
{
    if(usbHost.IsActiveClass(USBH_MIDI_CLASS))
    {
        hw.PrintLine("MIDI device class active");

        MidiUsbHandler::Config midi_config;
        midi_config.transport_config.periph
            = MidiUsbTransport::Config::Periph::HOST;
        midi.Init(midi_config);
        midi.EnableMidi2(&midi2);
        midi.StartReceive();

        /* Send CI Discovery after a short delay for the device to settle */
        System::Delay(100);
        SendCIDiscovery();
    }
}

void USBH_Error(void* data)
{
    hw.PrintLine("USB error");
}

/*--------------------------------------------------------------------+
 * Main
 *--------------------------------------------------------------------*/
int main(void)
{
    hw.Init();
    hw.StartLog(true);

    /* ---- MIDI 2.0 processor ---- */
    Midi2Processor::Config m2cfg;
    m2cfg.manufacturer_id = 0x7D;       /* prototyping */
    m2cfg.family_id       = 0x0001;
    m2cfg.model_id        = 0x0001;
    m2cfg.version_id      = 0x00010000;
    m2cfg.muid_seed       = 0xDA15;
    m2cfg.group           = 0;
    midi2.Init(m2cfg);

    /* Upscale: MIDI 1.0 or 2.0 -> unified MT 0x4 callbacks */
    midi2.GetDispatch()->upscale_mt2 = true;
    midi2.GetDispatch()->on_note_on  = OnNoteOn;
    midi2.GetDispatch()->on_note_off = OnNoteOff;
    midi2.GetDispatch()->on_cc       = OnCC;

    /* ---- USB Host ---- */
    USBHostHandle::Config usbh_config;
    usbh_config.connect_callback      = USBH_Connect;
    usbh_config.disconnect_callback   = USBH_Disconnect;
    usbh_config.class_active_callback = USBH_ClassActive;
    usbh_config.error_callback        = USBH_Error;
    usbHost.Init(usbh_config);
    usbHost.RegisterClass(USBH_MIDI_CLASS);

    hw.PrintLine("MIDI 2.0 Host ready");

    /* ---- Main loop ---- */
    while(1)
    {
        usbHost.Process();

        if(usbHost.IsActiveClass(USBH_MIDI_CLASS) && midi.RxActive())
        {
            midi.Listen();

            /* MIDI 1.0 polling still available if needed */
            while(midi.HasEvents())
            {
                midi.PopEvent(); /* consume -- callbacks handle everything */
            }
        }

        System::Delay(1);
    }
}
