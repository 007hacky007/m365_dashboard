// Minimal adapter to provide SSD1306Ascii-like API on top of U8g2
#pragma once

#include <Arduino.h>
#include <U8g2lib.h>

class U8g2DisplayAdapter {
public:
  // Construct for I2C 128x64 SSD1306. We use hardware I2C by default.
  U8g2DisplayAdapter()
  : u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE) {}

  // begin() signature variants used by existing code
  void begin(const void* /*driver*/, uint8_t i2cAddr) {
    (void)i2cAddr; // U8g2 handles address internally after setI2CAddress
    u8g2.setI2CAddress(i2cAddr << 1);
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tf);
    clear();
  }
  void begin(const void* /*driver*/, uint8_t /*cs*/, uint8_t /*dc*/, uint8_t /*rst*/) {
    // SPI path not used currently; keep for compatibility
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tf);
    clear();
  }

  void clear() {
    u8g2.clearBuffer();
    _xpx = 0; _rowPage = 0; _scale = 1; _usingStdNumb = false; _isDefaultFont = true; _isSeg = false;
  }

  // Font handling via sentinel pointers defined in fonts/u8g2_alias.h
  void setFont(const uint8_t* f) {
    if (!f) return;
    uint8_t id = f[0];
    _usingStdNumb = false;
    _isSeg = false;
  _isDefaultFont = false;
    switch (id) {
      case 0x02: // stdNumb
        u8g2.setFont(u8g2_font_logisoso16_tf);
        _usingStdNumb = true;
        break;
      case 0x03: // bigNumb
        u8g2.setFont(u8g2_font_logisoso24_tf);
        break;
      case 0x04: // segNumb (use a seven-seg style font)
        u8g2.setFont(u8g2_font_7Segments_26x42_mn);
        _isSeg = true;
        break;
      case 0x05: // m365 icon font -> fallback to default
      case 0x01: // defaultFont
      default:
        u8g2.setFont(u8g2_font_6x10_tf);
    _isDefaultFont = true;
        break;
    }
  }

  // 1X/2X scaling for default font only; for big/seg fonts we ignore and use their size
  void set1X() { _scale = 1; }
  void set2X() { _scale = 2; }

  // SSD1306Ascii setCursor expects X in pixels and Y as row page (8px steps)
  void setCursor(uint8_t col, uint8_t row) {
    _xpx = col;
    _rowPage = row;
  }

  // Print helpers. We draw into buffer and send on demand to reduce flicker.
  size_t print(char c) { return writeChar(c); }
  size_t print(const char* s) { size_t n=0; while (*s) { n += writeChar(*s++); } return n; }
  size_t print(const String& s) { return print(s.c_str()); }
  size_t print(const __FlashStringHelper* f) { return print(reinterpret_cast<const char*>(f)); }
  size_t print(int v) { char b[16]; itoa(v, b, 10); return print(b); }
  size_t print(unsigned v) { char b[16]; utoa(v, b, 10); return print(b); }
  size_t print(long v) { char b[24]; ltoa(v, b, 10); return print(b); }
  size_t print(unsigned long v) { char b[24]; ultoa(v, b, 10); return print(b); }
  size_t print(float v) { char b[32]; dtostrf(v, 0, 2, b); return print(b); }

  // Expose pixel X and row page Y used by existing code
  uint8_t col() const { return (uint8_t)constrain(_xpx, 0, 127); }
  uint8_t row() const { return _rowPage; }

  // Graphics used by battery bar rendering
  void drawBox(uint8_t x, uint8_t y, uint8_t w, uint8_t h) { u8g2.drawBox(x, y, w, h); }
  void drawFrame(uint8_t x, uint8_t y, uint8_t w, uint8_t h) { u8g2.drawFrame(x, y, w, h); }
  void drawRBox(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t r) { u8g2.drawRBox(x, y, w, h, r); }
  void drawRFrame(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t r) { u8g2.drawRFrame(x, y, w, h, r); }
  void drawVLine(uint8_t x, uint8_t y, uint8_t h) { u8g2.drawVLine(x, y, h); }
  void drawDisc(uint8_t x, uint8_t y, uint8_t r) { u8g2.drawDisc(x, y, r); }
  void drawTriangle(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) { u8g2.drawTriangle(x0, y0, x1, y1, x2, y2); }
  void setDrawColor(uint8_t c) { u8g2.setDrawColor(c); }

  // Push current buffer to the display (call once per frame)
  void commit() { u8g2.sendBuffer(); }

  // For compatibility in other places
  void displayRemap(bool /*v*/) {}

private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2; // full buffer for simplicity
  int16_t _xpx = 0;    // current X in pixels
  uint8_t _rowPage = 0; // current Y page (0..7), each page is 8px high
  uint8_t _scale = 1; // only affects default font
  bool _usingStdNumb = false;
  bool _isDefaultFont = true;

  void flush() { /* no-op: batching */ }
  size_t writeChar(char c) {
    // Choose drawing method based on font selection
    int x = _xpx;
    int yTop = (int)_rowPage * 8;
    // For U8g2, y is baseline. Offset baseline by ascent from the top of the 8px row page
    // so that text roughly fits within the 8px band when using small default font.
    // For larger fonts (std/big/seg), callers pass pixel X and page Y appropriate for layout.
    int ascent = u8g2.getAscent();
    int baseline = yTop + ascent;
    // Skip special placeholder used by original seg font to render blank
    if (_isSeg && c == ';') {
      // Advance by an approximate digit width for seg font
      _xpx += 24; // rough width of one 7-seg glyph
  // no flush, batched
      return 1;
    }
    // If default font at 2X, temporarily switch to a larger font for this draw
    bool swappedDefault = false;
    if (_isDefaultFont) {
      if (_scale == 2) {
        u8g2.setFont(u8g2_font_10x20_tf);
        swappedDefault = true;
      } else {
        u8g2.setFont(u8g2_font_6x10_tf);
      }
      ascent = u8g2.getAscent();
      baseline = yTop + ascent;
    }
    char s[2] = {c, 0};
    u8g2.drawStr(x, baseline, s);
    int w = u8g2.getStrWidth(s);
    _xpx += w;
    // Restore small default font if we changed it
    if (swappedDefault) {
      // Restore small default font for subsequent operations
      u8g2.setFont(u8g2_font_6x10_tf);
    }
  // no flush, batched
    return 1;
  }
  bool _isSeg = false;
};
