/*
 * usbkey-mega2560 — JieLi USB_KEY sender (0x16EF) for an Arduino Mega 2560:
 * forces the BR23 boot ROM into the USB bootloader (README.md).
 *
 * The PC stays wired to the controller the whole time. After the ACK the ROM
 * calibrates its clock from the PC's SOFs and only tries a few times, so the
 * PC must be on the bus within milliseconds of the ACK; moving cables by hand
 * is too slow (it then enumerates with a wrong clock: error -71). Instead this
 * board takes itself off the bus electronically the moment it sees the ACK.
 *
 * The Mega's pins are 5 V and the USB lines are 3.3 V:
 *  - the bus pins are open-drain: driven low or released, never driven high;
 *  - the pull-up for each line is a divider between two pins, 5 V -> 1k ->
 *    node -> 2k -> 0 V, so the node sits at 3.3 V. With both pins switched to
 *    inputs it is gone from the bus, which a pull-up to the 3V3 pin can't do.
 *  - the node voltage is checked at every start; above 3.6 V (2k missing)
 *    nothing is driven.
 * The ACK is sensed with the ADC: a 5 V AVR needs 3.0 V for a guaranteed
 * digital high.
 *
 * NOT YET TESTED ON HARDWARE.
 *
 * Board: Arduino Mega 2560. Serial monitor: 115200 baud. 'r' = restart keying.
 *
 * Per line (README.md has the diagram):
 *   D- node: A0, 1k to pin 22, 2k to pin 23, 220 Ohm to USB D- (white)
 *   D+ node: A1, 1k to pin 24, 2k to pin 25, 220 Ohm to USB D+ (green)
 *   GND -> USB GND (black). 13 (PB7) on-board LED: on = key acknowledged.
 * Never connect 5 V to the USB wires.
 */
#include <avr/io.h>

static const uint8_t BIT_DM = _BV(PF0);   // A0, ADC channel 0
static const uint8_t BIT_DP = _BV(PF1);   // A1, ADC channel 1

// Divider pull-ups on PORTA. Separate pins per line: a shared pin would
// bridge D+ and D- once released.
static const uint8_t PU_HI_DM = _BV(PA0);  // pin 22: 1k to the D- node
static const uint8_t PU_LO_DM = _BV(PA1);  // pin 23: 2k to the D- node
static const uint8_t PU_HI_DP = _BV(PA2);  // pin 24: 1k to the D+ node
static const uint8_t PU_LO_DP = _BV(PA3);  // pin 25: 2k to the D+ node
static const uint8_t PU_ALL = PU_HI_DM | PU_LO_DM | PU_HI_DP | PU_LO_DP;

static const uint16_t USB_KEY = 0x16EF;

// ADC (8-bit, AVcc = 5 V reference). During the ACK the node sits at about
// 0.8 V (the chip pulls the line, the divider pulls the node through 220).
static const uint8_t ADC_LOW  = 66;    // ~1.3 V
static const uint8_t ADC_HIGH = 128;   // ~2.5 V

// A real ACK lasts 1-2 ms. A host port reset holds the lines low for 10 ms
// or more; it must not count.
static const uint32_t ACK_MIN_US = 500, ACK_MAX_US = 5000;

// Before the first function: the Arduino build puts its generated prototypes
// there, and they use these types.
struct Mode {
  uint8_t clk, dat;   // PORTF bits
  uint16_t half_us;   // half bit period
  const char *name;
};

enum Sense { S_HIGH, S_LOW, S_MIXED };

// Open drain on PORTF: PORTF bits stay 0 (no internal pull-up to 5 V), so
// DDR=1 pulls the line low and DDR=0 releases it to the divider.
static inline void drive(uint8_t bit, bool hi) {
  if (hi) DDRF &= ~bit; else DDRF |= bit;
}
static inline void release(uint8_t bit) { DDRF &= ~bit; }

static void pullups_on() {
  PORTA = (PORTA & ~PU_ALL) | PU_HI_DM | PU_HI_DP;
  DDRA |= PU_ALL;
}
// PORT first, then DDR: the 1k pins never become inputs with the internal
// 5 V pull-up on.
static void pullups_off() {
  PORTA &= ~PU_ALL;
  DDRA &= ~PU_ALL;
}

static inline uint8_t adc_read(uint8_t bit) {
  ADMUX = _BV(REFS0) | _BV(ADLAR) | (bit == BIT_DM ? 0 : 1);
  ADCSRA |= _BV(ADSC);
  while (ADCSRA & _BV(ADSC)) {}
  return ADCH;
}

static const Mode MODES[] = {
  {BIT_DP, BIT_DM, 3,  "clock D+, data D-, 3 us"},    // both ACKs so far: clock D+
  {BIT_DP, BIT_DM, 30, "clock D+, data D-, 30 us"},
  {BIT_DP, BIT_DM, 10, "clock D+, data D-, 10 us"},
  {BIT_DM, BIT_DP, 10, "clock D-, data D+, 10 us"},
  {BIT_DM, BIT_DP, 3,  "clock D-, data D+, 3 us"},
  {BIT_DM, BIT_DP, 30, "clock D-, data D+, 30 us"},
};
static const int NMODES = sizeof(MODES) / sizeof(MODES[0]);
static const int WORDS_PER_BURST = 24;   // keys sent before switching mode

static bool keying = true;
static int mode = 0;
static bool locked = false;
static bool listening = false;       // 'l': lines released, report every level change (bring-up)          // '1'..'6': one mode only (a device that checks once per power-on)
static uint32_t high_windows = 0;   // the chip is powered and listening (line pulled up)
static uint32_t last_report = 0;

static void all_release() {
  release(BIT_DM);
  release(BIT_DP);
}

static void print_mv(uint16_t mv) {
  Serial.print(mv / 1000);
  Serial.print('.');
  uint16_t frac = mv % 1000;
  if (frac < 100) Serial.print('0');
  if (frac < 10) Serial.print('0');
  Serial.print(frac);
  Serial.print(F(" V"));
}

// Slow, 10-bit, averaged: the keying ADC setup (1 MHz, 8-bit) is fast but a
// few percent off, too coarse for a safety limit. Channel 0x1E is the 1.1 V
// bandgap, which gives the real AVcc (USB 5 V is often 4.7-5.1 V).
static uint16_t adc_avg10(uint8_t ch) {
  ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);   // 125 kHz
  ADMUX = _BV(REFS0) | ch;
  delay(2);
  uint32_t sum = 0;
  for (int i = 0; i < 17; i++) {
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC)) {}
    if (i) sum += ADC;                 // first conversion after a switch is discarded
  }
  ADCSRA = _BV(ADEN) | _BV(ADPS2);     // back to 1 MHz for keying
  return sum / 16;
}

// Released nodes with the dividers on: both must be at or below 3.6 V before
// anything is connected to the controller.
static bool check_pullups() {
  all_release();
  pullups_on();
  delay(2);
  uint32_t vcc = 1100UL * 1023 / adc_avg10(0x1E);   // bandgap is +-10%
  uint16_t dm = adc_avg10(0) * vcc / 1023, dp = adc_avg10(1) * vcc / 1023;
  Serial.print(F("pull-up check: D- node "));
  print_mv(dm);
  Serial.print(F(", D+ node "));
  print_mv(dp);
  Serial.print(F(" (expect about 3.3 V; Mega supply "));
  print_mv(vcc);
  Serial.println(F(")"));
  if (dm > 3600 || dp > 3600) {
    pullups_off();
    Serial.println(F("!!! above 3.6 V: a 2k resistor is missing or on the wrong pin."));
    Serial.println(F("!!! Not keying. Unplug the USB-C half, fix the wiring, send 'r'."));
    return false;
  }
  if (dm < 2500 || dp < 2500)
    Serial.println(F("note: below 2.5 V, check the 1k (pin 22/24) and 2k (pin 23/25) wiring."));
  return true;
}

// One key, MSB first: data changes while the clock is low, rising edge samples.
static void send_key(const Mode &m) {
  for (int i = 15; i >= 0; i--) {
    drive(m.dat, (USB_KEY >> i) & 1);
    delayMicroseconds(m.half_us);
    drive(m.clk, true);
    delayMicroseconds(m.half_us);
    drive(m.clk, false);
  }
}

// Between keys: keep the clock low (no stray rising edge), release the data
// line and sample it for ~120 us. The chip's ACK holds both lines low for
// 1-2 ms; a powered, listening chip leaves the line high.
static Sense sense(const Mode &m) {
  release(m.dat);
  delayMicroseconds(8);
  int lows = 0, highs = 0;
  for (int n = 0; n < 8; n++) {     // ~15 us per conversion
    uint8_t v = adc_read(m.dat);
    if (v < ADC_LOW) lows++;
    else if (v > ADC_HIGH) highs++;
  }
  drive(m.dat, false);
  if (highs == 8) return S_HIGH;
  if (lows == 8) return S_LOW;
  return S_MIXED;
}

// After a low window: stop keying and keep watching the released data line
// (clock still low) for ~300 us. The ACK lasts 1-2 ms, so a real one is
// still there; noise or a single slow bit is not.
static bool confirm_low(const Mode &m) {
  release(m.dat);
  delayMicroseconds(8);
  bool low = true;
  for (int n = 0; n < 20 && low; n++) {
    if (adc_read(m.dat) >= ADC_LOW) low = false;
  }
  drive(m.dat, false);
  return low;
}

// Watch the released data line until it is high again, up to 50 ms. Returns
// the low time in us (including confirm_low's ~300), or 0 if it stayed low.
// Both lines are left released.
static uint32_t low_duration_us(const Mode &m) {
  release(m.clk);
  release(m.dat);
  uint32_t t0 = micros();
  while (micros() - t0 < 50000UL) {
    if (adc_read(m.dat) > ADC_HIGH) return (micros() - t0) + 300;
  }
  return 0;
}

static void start_keying() {
  digitalWrite(LED_BUILTIN, LOW);
  if (!locked) mode = 0;
  high_windows = 0;
  keying = check_pullups();
  if (keying)
    Serial.println(F("\nkeying: sending 0x16EF; now reset or power the controller (see README)"));
}

static void acknowledged(const Mode &m, uint32_t low_us) {
  all_release();
  pullups_off();                     // first: the ROM is already waiting for SOFs
  digitalWrite(LED_BUILTIN, HIGH);
  keying = false;
  Serial.print(F("\nACK received (mode: "));
  Serial.print(m.name);
  Serial.print(F(", line low for "));
  Serial.print(low_us);
  Serial.println(F(" us)."));
  Serial.println(F("This board is off the bus; the PC should see the bootloader now."));
  Serial.println(F("On the PC: lsusb | grep 4c4a  ->  sudo ./jlflash.sh write <v19 image>"));
  Serial.println(F("Send 'r' to key again."));
}

void setup() {
  PORTF &= ~(BIT_DM | BIT_DP);       // never enable the 5 V internal pull-ups
  all_release();
  pullups_off();
  DIDR0 = BIT_DM | BIT_DP;           // analog only: no digital input buffer
  ADCSRA = _BV(ADEN) | _BV(ADPS2);   // ADC on, clock 16 MHz / 16 = 1 MHz
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  delay(50);
  start_keying();
}

// Listen mode: both lines released (no pull-ups), each D+ / D- level change
// printed once it has held for ~1 ms, with how long the old level lasted.
// For a device that signals on its USB pins (the C2 receiver's bring-up
// checkpoints, docs/dongle.md).
static uint8_t lv[2] = {2, 2}, cand[2] = {2, 2}, cnt[2];
static uint32_t since[2];
static void start_listening() {
  keying = false;
  all_release();
  pullups_off();
  listening = true;
  lv[0] = lv[1] = cand[0] = cand[1] = 2;
  Serial.println(F("\nlistening: lines released; printing D+ / D- level changes ('r' to key again)"));
}
static void listen_step() {
  static const uint8_t bits[2] = {BIT_DP, BIT_DM};
  static const char *const names[2] = {"D+", "D-"};
  for (int i = 0; i < 2; i++) {
    uint8_t v = adc_read(bits[i]) > 85;          // 8-bit of 5 V: above ~1.65 V is high
    if (v == lv[i]) { cand[i] = 2; continue; }
    if (v != cand[i]) { cand[i] = v; cnt[i] = 0; }
    if (++cnt[i] < 20) continue;                 // held ~1 ms
    uint32_t now = millis();
    Serial.print(now); Serial.print(F(" ms  ")); Serial.print(names[i]);
    Serial.print(v ? F(" -> high") : F(" -> low"));
    if (lv[i] != 2) { Serial.print(F("  (after ")); Serial.print(now - since[i]); Serial.print(F(" ms)")); }
    Serial.println();
    lv[i] = v; since[i] = now; cand[i] = 2;
  }
}

void loop() {
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'r' || c == 'R') { listening = false; start_keying(); }
    if (c == 'l' || c == 'L') start_listening();
    if (c >= '1' && c < '1' + NMODES) {
      locked = true;
      mode = c - '1';
      Serial.print(F("\nlocked to mode "));
      Serial.println(MODES[mode].name);
      start_keying();
    }
    if (c == 'a' || c == 'A') {
      locked = false;
      Serial.println(F("\nrotating through all modes"));
      start_keying();
    }
  }
  if (listening) {
    listen_step();
    return;
  }
  if (!keying) {
    delay(10);
    return;
  }

  const Mode &m = MODES[mode];
  drive(m.clk, false);
  drive(m.dat, false);
  delayMicroseconds(m.half_us);
  for (int w = 0; w < WORDS_PER_BURST; w++) {
    send_key(m);
    Sense s = sense(m);
    if (s == S_HIGH) {
      high_windows++;
    } else if (s == S_LOW && high_windows >= 3) {
      // Only after the line has been seen high: an unpowered chip also reads
      // low (through its protection diodes), which must not count as an ACK.
      if (confirm_low(m)) {
        uint32_t low_us = low_duration_us(m);
        if (low_us >= ACK_MIN_US && low_us <= ACK_MAX_US) {
          acknowledged(m, low_us);
          return;
        }
        drive(m.clk, false);
        drive(m.dat, false);
        Serial.print(F("\nline low "));
        if (low_us) { Serial.print(low_us); Serial.print(F(" us")); }
        else Serial.print(F("> 50 ms"));
        Serial.println(F(": not an ACK (host reset or the chip holding it). Still keying."));
        high_windows = 0;   // count highs again before trusting a low
      }
    }
  }
  if (!locked) mode = (mode + 1) % NMODES;

  if (millis() - last_report > 2000) {
    last_report = millis();
    Serial.print(F("keying... line seen high in "));
    Serial.print(high_windows);
    Serial.println(high_windows ? F(" windows (chip listening, or lines not connected)")
                                : F(" windows (chip not powered yet?)"));
  }
}
