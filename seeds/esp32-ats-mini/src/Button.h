/*
 * Button.h — debounced push-button tracker (click / short / long press).
 *
 * Ported verbatim from the ats-mini project (github.com/esp32-si4732/ats-mini),
 * MIT-licensed, Copyright (c) 2025 ESP32-SI4732 Radio. Compatible with this
 * seed's MIT license. Only the Common.h include in Button.cpp was swapped for
 * <Arduino.h> (the tracker needs nothing beyond millis()).
 */
#ifndef BUTTON_H
#define BUTTON_H

#define DEBOUNCE_INTERVAL    50
#define SHORT_PRESS_INTERVAL 500
#define LONG_PRESS_INTERVAL  2000

class ButtonTracker {
  public:
    struct State {
      bool isPressed;       // Current pressed state (after debounce)
      bool wasClicked;      // Released after <0.5s press
      bool wasShortPressed; // Released after >0.5s press
      bool isLongPressed;   // Still pressed after >2s
    };

  ButtonTracker();
  void reset();
  State update(bool currentState, unsigned int debounceInterval = DEBOUNCE_INTERVAL);

  private:
    bool lastState; // Last raw input state
    bool lastStableState; // Last debounced state
    unsigned long lastDebounceTime;
    unsigned long pressStartTime;
};

#endif
