/** Baseline: USB MIDI Device without midi2 (stock libDaisy only)
 *  Used to test if Windows recognizes the Daisy as MIDI device.
 */
#include "daisy_seed.h"

using namespace daisy;

DaisySeed      hw;
MidiUsbHandler midi;

int main(void)
{
    hw.Init();

    MidiUsbHandler::Config midi_config;
    midi_config.transport_config.periph
        = MidiUsbTransport::Config::Periph::INTERNAL;
    midi.Init(midi_config);
    midi.StartReceive();

    while(1)
    {
        midi.Listen();

        while(midi.HasEvents())
        {
            MidiEvent msg = midi.PopEvent();
            if(msg.type == NoteOn)
            {
                uint8_t echo[3] = {0x90, msg.data[0], msg.data[1]};
                midi.SendMessage(echo, 3);
            }
        }

        System::Delay(1);
    }
}
