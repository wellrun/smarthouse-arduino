#include <Arduino.h>

#include <EEPROM.h>
#include <TimerOne.h>
#include <Logging.h>
#include <NewButton.h>
#include "Relay.h"

#define MY_NODE_ID  20
#define MY_RADIO_NRF24
#include <MySensors.h>

#define VERSION "0.3"

#define PIN_IN_ZERO_CROSS          2
#define PIN_OUT_AC                 3
#define PIN_OUT_RELAY1             4
#define PIN_IN_BUTTON              5
#define PIN_OUT_RELAY2             6
#define PIN_IN_RELAY1_BUTTON       7
#define PIN_IN_RELAY2_BUTTON       8

#define SERIAL_SPEED            115200U

#define MS_LAMP_ID              0
#define MS_RELAY1_ID            1
#define MS_RELAY2_ID            2

// This is the delay-per-brightness step in microseconds. It allows for 128 steps
// If using 60 Hz grid frequency set this to 65
#define FREQ_STEP               75

// Delay for transition at button long press
#define FADE_DELAY_SLOW_MS           50
// Delay for transition between on and off
#define FADE_DELAY_FAST_MS           20

#define DIMMER_HIGH   120
#define DIMMER_MAX    100
#define DIMMER_LOW    0

#define BUTTON_SHORT_PRESS_TIME 100
#define BUTTON_LONG_PRESS_TIME 2000

#define EEPROM_OFFSET 10

bool button_direction = true;

struct {
    volatile byte actual;
    byte limit;
    byte target;
    unsigned int delay;
} dimmer = {DIMMER_MAX, DIMMER_MAX, DIMMER_MAX, FADE_DELAY_SLOW_MS};

volatile int step_counter = 0;     // Variable to use as a counter of dimming steps. It is volatile since it is passed between interrupts
volatile bool zero_cross = false;  // Flag to indicate we have crossed zero

MyMessage msgLampLightStatus(MS_LAMP_ID, V_LIGHT);
MyMessage msgLampLightLevel(MS_LAMP_ID, V_LIGHT_LEVEL);

NewButton button{PIN_IN_BUTTON};
Relay relays[] = {Relay{PIN_IN_RELAY1_BUTTON, PIN_OUT_RELAY1},
                  Relay{PIN_IN_RELAY2_BUTTON, PIN_OUT_RELAY2}};

void on_btn_long_press();
void on_btn_short_release();
void on_btn_long_release();

void presentation()
{
    LOG_INFO("MySensors setup...");
    sendSketchInfo("LightSwitchAC", VERSION);
    present(MS_LAMP_ID, S_LIGHT, "Lamp");
    present(MS_RELAY1_ID, S_LIGHT, "Relay 1");
    present(MS_RELAY2_ID, S_LIGHT, "Relay 2");
}

void setup()
{
    Serial.begin(SERIAL_SPEED);
    LOG_DEBUG("Setup has been started.")

    LOG_DEBUG("EEPROM restore...");
    eeprom_restore();

    LOG_DEBUG("Button setup");
    button.setup();
    button.on_long_press(on_btn_long_press);
    button.on_short_release(on_btn_short_release);
    button.on_long_release(on_btn_long_release);

    LOG_INFO("Pins setup...");
    pinMode(PIN_OUT_AC, OUTPUT);                          // Set the Triac pin as output

    LOG_INFO("Interrupts setup...");
    attachInterrupt(digitalPinToInterrupt(PIN_IN_ZERO_CROSS), on_zero_cross_detect, RISING);
    Timer1.initialize(FREQ_STEP);                      // Initialize TimerOne library for the freq we need
    Timer1.attachInterrupt(on_timer_dim_check, FREQ_STEP);      // Go to dim_check procedure every 75 uS (50Hz)  or 65 uS (60Hz)

    LOG_INFO("Setup done");
}

void loop()
{
    dimmer_process();
    button.process();

    relays[0].process();
    relays[1].process();
}

bool change_button_direction(byte min, byte max)
{
    bool ret = false;

    if (dimmer.actual >= max) {
        ret = false;
    }
    else if (dimmer.actual <= min) {
        ret = true;
    }
    else {
        ret = !button_direction;
    }
    LOG_DEBUG("Going %s", ret ? "up" : "down");

    return ret;
}

void on_btn_long_press(void *)
{
    button_direction = change_button_direction(DIMMER_LOW, DIMMER_MAX);
    if (button_direction) {
        dimmer.target = DIMMER_MAX;
    }
    else {
        dimmer.target = DIMMER_LOW;
    }
    dimmer.delay = FADE_DELAY_SLOW_MS;
}

void on_btn_short_release(void *)
{
    LOG_DEBUG("Button: short release");
    button_direction = change_button_direction(dimmer.limit, DIMMER_HIGH);
    dimmer_switch(button_direction, FADE_DELAY_FAST_MS);
}

void on_btn_long_release(void *)
{
    LOG_DEBUG("Button: long release. Stopped at %d", dimmer.actual);
    dimmer_level(dimmer.actual);
}

void on_zero_cross_detect()
{
    zero_cross = true;
    step_counter = 0;
    digitalWrite(PIN_OUT_AC, LOW);
}

void on_timer_dim_check()
{
    if (zero_cross && dimmer.actual >= DIMMER_MAX) {
        if (step_counter >= dimmer.actual) {
            digitalWrite(PIN_OUT_AC, HIGH);  // turn on light
            step_counter = 0;  // reset time step counter
            zero_cross = false;    // reset zero cross detection flag
        }
        else {
            step_counter++;  // increment time step counter
        }
    }
}

void eeprom_restore()
{
    dimmer.limit = EEPROM.read(EEPROM_OFFSET);
    if (dimmer.limit > DIMMER_MAX) {
        dimmer.limit = DIMMER_MAX;
    }
    dimmer.actual = DIMMER_HIGH;
    dimmer.target = DIMMER_HIGH;
    LOG_DEBUG("EEPROM restored: limit=%d", dimmer.limit);
}

void eeprom_save()
{
    EEPROM.update(EEPROM_OFFSET, dimmer.limit);
    if (EEPROM.read(EEPROM_OFFSET) != dimmer.limit) {
        LOG_ERROR("EEPROM write failure");
        return;
    }

    LOG_DEBUG("EEPROM saved: limit=%d", dimmer.limit);
}

void dimmer_switch(bool status, unsigned int delay)
{
    if (status) {
        dimmer.target = DIMMER_HIGH;
        send_message(msgLampLightStatus.set(0));
    }
    else {
        dimmer.target = dimmer.limit;
        send_message(msgLampLightStatus.set(1));
    }
    LOG_DEBUG("Switch dimmer to %d", dimmer.target);
    dimmer.delay = delay;
}

// Going from actual to target level with delay
void dimmer_process()
{
    static unsigned long last_now = millis();

    bool direction = dimmer.target > dimmer.actual;
    if (direction && (dimmer.actual >= dimmer.target)) {
        return;
    }
    else if (!direction && (dimmer.actual == DIMMER_LOW)) {
        return;
    }
    else if (dimmer.actual == dimmer.target) {
        return;
    }

    unsigned long now = millis();
    if (now - last_now > dimmer.delay) {
        dimmer.actual += (direction ? +1 : -1);
        last_now = now;

        LOG_DEBUG("dimmer: %u/%u", dimmer.actual, dimmer.target);
    }
}

void send_message(MyMessage & message)
{
    if (!send(message)) {
        LOG_ERROR("Message (sensor=%d, type=%d) doesn't reach a next node", message.sensor, message.type);
    }
}

void receive(const MyMessage & message)
{
    if (message.sensor == MS_LAMP_ID) {
        if (message.type == V_LIGHT_LEVEL) {
            on_message_set_light_level(message);
        }
        else if (message.type == V_LIGHT) {
            on_message_set_lamp_status(message);
        }
        else if (message.type == V_VAR1) {
            on_message_dump_data(message);
        }
        else {
            LOG_ERROR("Got message with unexpected type: (sensor=%d, type=%d)", message.sensor,
                      message.type);
        }
    }
    else if (message.sensor == MS_RELAY1_ID || message.sensor == MS_RELAY2_ID) {
        on_message_set_relay_status(message);
    }
    else {
        LOG_ERROR("Got message from unexpected sensor: (sensor=%d, type=%d)", message.sensor, message.type);
        return;
    }
}

void on_message_set_light_level(const MyMessage & message)
{
    int light_level = constrain(message.getInt(), DIMMER_LOW, DIMMER_MAX);
    LOG_DEBUG("=> Message: sensor=%d, type=LIGHT_LEVEL, value=%d", message.sensor, light_level);

    byte level = light_dimmer_convert(light_level);
    dimmer_level(level);
}

void dimmer_level(byte level)
{
    LOG_DEBUG("set limit = %d", level);
    dimmer.limit = level;
    dimmer.target = level;

    eeprom_save();

    int light_level = light_dimmer_convert(level);
    send_message(msgLampLightLevel.set(light_level));
    send_message(msgLampLightStatus.set(1));
}

void on_message_set_lamp_status(const MyMessage & message)
{
    bool light_status = message.getBool();
    LOG_DEBUG("=> Message: sensor=%d, type=LIGHT, value=%d", message.sensor, light_status);

    dimmer_switch(!light_status, FADE_DELAY_FAST_MS);
}

void on_message_dump_data(const MyMessage & message)
{
    LOG_DEBUG("=> Message: sensor=%d, type=VAR1", message.sensor);

    LOG_DEBUG("Actual: %d", dimmer.actual);
    LOG_DEBUG("Target: %d", dimmer.target);
    LOG_DEBUG("Max: %d", dimmer.limit);
    LOG_DEBUG("Delay: %ums", dimmer.delay);
    LOG_DEBUG("Button direction: %s", button_direction ? "up" : "down");
}

void on_message_set_relay_status(const MyMessage & message)
{
    bool status = message.getBool();
    LOG_DEBUG("=> Message: sensor=%d, type=LIGHT, value=%d", message.sensor, status);

    if (message.sensor == MS_RELAY1_ID) {
        relays[0].set_status(status);
    }
    else if (message.sensor == MS_RELAY2_ID) {
        relays[1].set_status(status);
    }
}

byte light_dimmer_convert(byte level)
{
    return DIMMER_MAX - level;
}
