#ifndef ARDUINO_RELAY_H
#define ARDUINO_RELAY_H

#include <Arduino.h>
#include <NewButton.h>

class Relay
{
public:
    Relay(byte input_pin, byte output_pin);

    void switch_status();
    void set_status(bool status);

    void process();

private:
    static void on_button_short_release(void * data);

    bool m_status;
    NewButton m_button;
    byte m_output_pin;
};

#endif //ARDUINO_RELAY_H
