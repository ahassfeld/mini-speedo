/*
  Speedo with multifunction display

  Discussed here:
  https://www.mini-forum.de/forum/thread/95593-elektronischer-mitteltacho/

  Inspired by: https://retromini.weebly.com/blog/arduino-speedometer#

  Complete rewrite, though.

  Uses an Arduino Nano, a X27 stepper motor and a 0,96" OLED SSD1306 display

  jack union, 2020.

*/

#define MINI_SPEEDO_VERSION "Speedo v0.7"

//----Libraries to Include--------
#include <Arduino.h>
#include <Wire.h>
// edited in clib/u8g2.h
//#define U8G2_WITH_CLIP_WINDOW_SUPPORT
//#define U8G2_WITH_FONT_ROTATION
//#define U8G2_WITH_UNICODE
#include "SwitecX25.h"
#include <AltSoftSerial.h>
#include <EEPROMex.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <avr/wdt.h>
#define REQUIRESNEW false
#define REQUIRESALARMS false
#include <DallasTemperature.h>
//----End Libraries--------------

//----Define PIN Settings----------
#define INPUT_SPEED 2   // Interruptable PIN on Nano
#define INPUT_RPM 3     // Interruptable PIN on Nano
#define STEPPIN_1 4     // back view top right pin
#define STEPPIN_2 5     // back view bottom right pin
#define STEPPIN_3 6     // back view bottom left pin
#define STEPPIN_4 7     // back view top left pin
#define SERIAL_RX 8     // D8 lambda digital serial input
#define SERIAL_TX 9     // D9 lambda digital serial input
#define INPUT_BUTTON 10 // brake light test button
#define INPUT_POWER 11  // Power sense pin
#define OUTPUT_POWER 12 // Power off pin
#define OUTPUT_WARN 13  // D13 warn light out

#define INPUT_OUTSIDETEMP A0
#define INPUT_OILTEMP A1
#define INPUT_OILPRESS A2
#define INPUT_LAMBDA A3
// A4: SDA
// A5: SCL
#define INPUT_WATERTEMP A6
#define INPUT_VOLTAGE A7
//----End Define PIN Settings------

//----Define Lambda sensor input----
//#define LAMBDA_DIGITAL true //RX and TX serial pins
#define LAMBDA_DIGITAL false // analog 5V input
//----End Define Lambda sensor input----

//----Define warning limits----
#define WARN_MAX_OILTEMP 120   // 120° Celsius
#define WARN_MIN_OILPRESS 100  // 1 bar pressure
#define WARN_MIN_VOLTAGE 118   // 11.8 Volts
#define WARN_MAX_VOLTAGE 154   // 15.4 Volts
#define WARN_MAX_WATERTEMP 100 // 100° Celsius
#define WARN_MIN_OUTSIDETEMP 2 // 2° Celsius
//----End Define warning limits----

//----Define OLED Display Settings----
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
//-----End OLED Display Settings------

//----Define other constants-------
#define SPEED_IMP_PER_REV 6 // impulses per revolution
#define IMP_PER_KM 800      // "Wegstrecke", impulses per 1000m
#define RPM_IMP_PER_REV 2   // SPI/Distributor, set to 1 for MPI
#define UPDATE_INTERVAL 100 // milliseconds speedo update rate
#define TEMP_EVERY_LOOPS 10 // every 10 loops update Dallas temperature sensor
//----End Define other constants---

//----Define sensor constants-------
#define ANALOGSAMPLES 5
#define MINITEMP_T0 303     // 303 Kelvin = 30° Celsius
#define MINITEMP_R0 833     // 833 Ohm at 303 Kelvin
#define MINITEMP_B 3540     // beta constant for Steinhart Hart
#define RACIMEXTEMP_T0 318  // 318 Kelvin = 45° Celsius
#define RACIMEXTEMP_R0 1240 // 1240 Ohm at 318 Kelvin
#define RACIMEXTEMP_B 3850  // beta constant for Steinhart Hart
#define OILTEMP_PULLUP 470  // 470 Ohm to 5V
#define WATERTEMP_PULLUP 61 // Resistance of water temp instrument
#define OILPRESS_PULLUP 100 // 100 Ohm to 5V
#define NODATA 65535        // max uint16 when no data
#define TEMPTOOHIGH 65534   // max uint16 -1 when reading too high
//----End Define sensor constants---

//----Stepper settings and object----
// standard X25.168 range 315 degrees at 1/3 degree steps
#define STEPS 248 * 3
// speedo type
#define SPEEDO_RANGE 210 // cooper with 210kph full range
#define SWEEP_AT_START 1
//----End Stepper settings and object----

//----Display modes----
#define TRIP 0
#define SPEED 1
#define LAMBDA 2
#define OIL_TEMP 3
#define OIL_PRESS 4
#define WATER_TEMP 5
#define VOLT 6
#define OUTSIDE 7
#define RPM 8
#define MINI 9

#define MODEMAX 9
//----End Display modes----

//----Display mode names----
const char *displayNames[] = {
    "trip",  "speed", "lambda",  "oil temp", "oil press",
    "water", "volt",  "outside", "rpm",
    "" // no text in logo mode
};
//----End Display mode names----

//----Define Display settings----
#define X_OFFSET 10
//----End Define Display settings----

//----EERPOM positions----
#define EE_TOKEN_POS 0
#define EE_TOKEN_PATTERN 170 // 10101010
#define EE_TOTAL_POS 1
#define EE_TRIP_POS 5
//----End EERPOM positions----

//----Objects----
// 1,3" SH1106 display
// U8G2_SH1106_128X64_NONAME_2_HW_I2C display(U8G2_R0);
// 0,96" SSD1306 display
U8G2_SSD1306_128X64_NONAME_2_HW_I2C bigDisplay(U8G2_R2);
// 0,96" SSD1306 additional display
U8G2_SSD1306_128X64_NONAME_2_HW_I2C smallDisplay(U8G2_R0);

// create the motor object with the maximum steps allowed
SwitecX25 stepper(STEPS, STEPPIN_1, STEPPIN_2, STEPPIN_3, STEPPIN_4);

// Serial input port
AltSoftSerial altSerial;

// External temperature
OneWire oneWire(INPUT_OUTSIDETEMP);
DallasTemperature outsideSensor(&oneWire);
DeviceAddress dallasDeviceAddress;
//----End Objects----

//-----Variables----------------------
uint32_t total = 0;       // total distance in decimeters
uint32_t trip = 0;        // trip distance in decimeters
uint16_t speed = 0;       // speed in kph
uint16_t lambda = 0;      // lambda factor *100
uint16_t oiltemp = 0;     // temperature in °C
uint16_t oilpress = 0;    // oil pressure in bar *100
uint16_t voltage = 0;     // main voltage in volt *10
uint16_t watertemp = 0;   // temperature in °C
uint16_t outsidetemp = 0; // temperature in °C *10
uint16_t rpm = 0;         // revolutions per minute

bool warningOiltemp = false;
bool warningOilpress = true; // probably no oil pressure at startup
bool warningVoltage = false;
bool warningWatertemp = false;
bool warningOutsidetemp = true; // no real startvalue for TEMP_EVERY_LOOPS

#define ROLLOVER 1000000000 // 100000km in decimeter
byte displayMode = TRIP;    // Startup setting
bool buttonState = HIGH;
bool buttonBeforeState = HIGH;
unsigned long timePressed = 0;
unsigned long displayUpdatedAt = 0;
bool displayChanged = false;
byte loops = 0;

#define ZEROTIME 1000 // no new interrupt pulses for max ms
volatile unsigned long rpmCount = 0;
volatile unsigned long rpmEvent = 0;
unsigned long rpmLastEvent = 0;
volatile unsigned long speedCount = 0;
volatile unsigned long speedEvent = 0;
unsigned long speedLastEvent = 0;

//----End Variables-------------------

//-----Start-up code run once---------
void setup(void) {
  MCUSR = 0;     // clear status register
  wdt_disable(); // watchdog disable

  Serial.begin(115200);
  altSerial.begin(115200);
  Serial.println(F(MINI_SPEEDO_VERSION));

  pinMode(INPUT_SPEED, INPUT_PULLUP);
  pinMode(INPUT_RPM, INPUT_PULLUP);
  pinMode(INPUT_BUTTON, INPUT_PULLUP);
  pinMode(INPUT_POWER, INPUT); // power sense pin
  pinMode(OUTPUT_POWER, OUTPUT);
  digitalWrite(OUTPUT_POWER, LOW); // power on
  pinMode(OUTPUT_WARN, OUTPUT);
  digitalWrite(OUTPUT_WARN, LOW); // warning off

  initOutsideSensor();
  initDisplays();
  delay(1000);
  reset_stepper();

  EEPROM.setMemPool(0, EEPROMSizeNano); // set memory size
  load_from_eeprom();

  attachInterrupt(digitalPinToInterrupt(INPUT_SPEED), interrupt_speed, RISING);
  attachInterrupt(digitalPinToInterrupt(INPUT_RPM), interrupt_rpm, RISING);

  Serial.println(F("Setup done."));
}
//------End Start-up code-------------

//------Start of Loop-----------------
void loop() {
  // check button status
  do_button();

  // display and data refresh every (UPDATE_INTERVAL) milliseconds
  if ((millis() - displayUpdatedAt) > UPDATE_INTERVAL) {
    gather_data();
    update_speed();
    update_rpm();
    check_warnings();
    do_display();
    do_stepper();
    displayUpdatedAt = millis();
    loops = loops + 1;
  }

  // refresh data from Dallas sensor less often
  if (loops == TEMP_EVERY_LOOPS) {
    outsidetemp = int(outsideSensor.getTempCByIndex(0) * 10);
    outsideSensor.requestTemperatures(); // request value for next run
    loops = 0;
  }

  // move stepper towards target position
  stepper.update();

  // switch power supply off and save to eeprom
  // when power goes down
  sense_power_off();
}
//-------End of Loop------------------

//-------Start interrupt handling-----
void interrupt_speed() {
  speedCount++;
  speedEvent = millis();
}

void interrupt_rpm() {
  rpmCount++;
  rpmEvent = millis();
}
//-------End interrupt handling-----

//-------Start of Functions-----------
void do_button() {
  buttonState = digitalRead(INPUT_BUTTON);
  if (buttonState == LOW) {
    if (buttonBeforeState == HIGH) { // press started
      timePressed = millis();
      buttonBeforeState = LOW;
    } else {                               // still holding LOW
      if (millis() - timePressed > 1000) { // one second hold
        if (displayMode == TRIP) {
          trip = 0; // reset trip
        }
      }
    }
  } else {                          // buttonState HIGH
    if (buttonBeforeState == LOW) { // just released
      if ((millis() - timePressed > 50) and
          (millis() - timePressed < 1000)) { // short press ended
        increment_display();
        displayChanged = true;
      }
      buttonBeforeState = HIGH;
    }
  }
}

void initOutsideSensor() {
  outsideSensor.begin();
  outsideSensor.getAddress(dallasDeviceAddress, 0);
  outsideSensor.setResolution(dallasDeviceAddress,
                              9);            // less resolution, faster reading
  outsideSensor.setWaitForConversion(false); // async reading
  outsideSensor.requestTemperatures();       // start first request
}

void sense_power_off() {
  if (digitalRead(INPUT_POWER) == LOW) {
    save_to_eeprom();
    stepper.setPosition(0);
    stepper.updateBlocking();
    bigDisplay.firstPage();
    do {
      draw_goodbye();
    } while (bigDisplay.nextPage());
    delay(2000);
    digitalWrite(OUTPUT_POWER, HIGH);
    stop();
  }
}

void load_from_eeprom() {
  if (EEPROM.readByte(EE_TOKEN_POS) == EE_TOKEN_PATTERN) { // found pattern?
    total = EEPROM.readLong(EE_TOTAL_POS);
    trip = EEPROM.readLong(EE_TRIP_POS);
  } else { // first start, save zero values
    EEPROM.writeByte(EE_TOKEN_POS, EE_TOKEN_PATTERN);
    EEPROM.writeLong(EE_TOTAL_POS, 0);
    EEPROM.writeLong(EE_TRIP_POS, 0);
  }
}

void save_to_eeprom() {
  // When? At least at stop.
  // The EEPROM memory has a specified life of 100,000 write/erase cycles
  // An EEPROM write takes 3.3 ms to complete.
  EEPROM.updateLong(EE_TOTAL_POS, total);
  EEPROM.updateLong(EE_TRIP_POS, trip);
}

void stop() {
  delay(1000);           // wait a second
  wdt_enable(WDTO_15MS); // enable watchdog
  for (;;)
    ; // are we still there? wait for watchdog to reset us.
}
