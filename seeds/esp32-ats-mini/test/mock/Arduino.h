/* Minimal host-side stub of the Arduino API, just enough to compile and drive
 * Rotary.cpp on a desktop with plain g++/clang -- no ESP32, no PlatformIO.
 * The test defines pinMode/digitalRead over a fake pin array so it can feed the
 * decoder synthetic quadrature pin sequences. MIT, part of the seed firmware. */
#ifndef MOCK_ARDUINO_H
#define MOCK_ARDUINO_H

#define INPUT        0x0
#define INPUT_PULLUP 0x2
#define HIGH         0x1
#define LOW          0x0

// Backed by the test translation unit (see test_rotary.cpp).
void pinMode(int pin, int mode);
int digitalRead(int pin);

#endif
