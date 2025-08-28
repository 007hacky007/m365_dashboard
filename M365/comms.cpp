#include "comms.h"

void dataFSM() {
  static uint8_t   step = 0, _step = 0, entry = 1;
  static uint32_t   beginMillis;
  static uint8_t   Buf[RECV_BUFLEN];
  static uint8_t* _bufPtr;
  _bufPtr = (uint8_t*)&Buf;

  switch (step) {
    case 0:
      while (XIAOMI_PORT.available() >= 2)
        if (XIAOMI_PORT.read() == 0x55 && XIAOMI_PORT.peek() == 0xAA) {
          XIAOMI_PORT.read();
          step = 1;
          break;
        }
      break;
    case 1: {
      static uint8_t   readCounter;
      static uint16_t    _cs;
      static uint8_t* bufPtr;
      static uint8_t* asPtr;
      uint8_t bt;
      if (entry) {
        memset((void*)&AnswerHeader, 0, sizeof(AnswerHeader));
        bufPtr = _bufPtr;
        readCounter = 0;
        beginMillis = millis();
        asPtr = (uint8_t*)&AnswerHeader;
        _cs = 0xFFFF;
      }
      if (readCounter >= RECV_BUFLEN) { step = 2; break; }
      if (millis() - beginMillis >= RECV_TIMEOUT) { step = 2; break; }
      while (XIAOMI_PORT.available()) {
        bt = XIAOMI_PORT.read();
        readCounter++;
        if (readCounter <= sizeof(AnswerHeader)) {
          *asPtr++ = bt;
          _cs -= bt;
        }
        if (readCounter > sizeof(AnswerHeader)) {
          *bufPtr++ = bt;
          if(readCounter < (AnswerHeader.len + 3)) _cs -= bt;
        }
        beginMillis = millis();
      }
      if (AnswerHeader.len == (readCounter - 4)) {
        uint16_t cs;
        uint16_t* ipcs;
        ipcs = (uint16_t*)(bufPtr-2);
        cs = *ipcs;
        if(cs != _cs) { step = 2; break; }
        processPacket(_bufPtr, readCounter);
        step = 2;
        break;
      }
      break; }
    case 2:
      step = 0;
      break;
  }

  if (_step != step) { _step = step; entry = 1; } else entry = 0;
}

void processPacket(uint8_t* data, uint8_t len) {
  uint8_t RawDataLen;
  RawDataLen = len - sizeof(AnswerHeader) - 2;

  switch (AnswerHeader.addr) {
    case 0x20:
      switch (AnswerHeader.cmd) {
        case 0x00:
          switch (AnswerHeader.hz) {
            case 0x64:
              break;
            case 0x65:
              if (_Query.prepared == 1 && !_Hibernate) writeQuery();
              memcpy((void*)& S20C00HZ65, (void*)data, RawDataLen);
              break;
            default:
              break;
          }
          break;
        default:
          break;
      }
      break;
    case 0x21:
      switch (AnswerHeader.cmd) {
        case 0x00:
        switch(AnswerHeader.hz) {
          case 0x64:
            memcpy((void*)& S21C00HZ64, (void*)data, RawDataLen);
            break;
          }
          break;
      default:
        break;
      }
      break;
    case 0x22:
      break;
    case 0x23:
      switch (AnswerHeader.cmd) {
        case 0x3E:
          if (RawDataLen == sizeof(A23C3E)) 
            memcpy((void*)& S23C3E, (void*)data, RawDataLen);
          break;
        case 0xB0:
          if (RawDataLen == sizeof(A23CB0)) 
            memcpy((void*)& S23CB0, (void*)data, RawDataLen);
          break;
        case 0x23:
          if (RawDataLen == sizeof(A23C23)) 
            memcpy((void*)& S23C23, (void*)data, RawDataLen);
          break;
        case 0x3A:
          if (RawDataLen == sizeof(A23C3A)) 
            memcpy((void*)& S23C3A, (void*)data, RawDataLen);
          break;
        default:
          break;
      }
      break;
    case 0x25:
      switch (AnswerHeader.cmd) {
        case 0x40:
          if(RawDataLen == sizeof(A25C40)) 
            memcpy((void*)& S25C40, (void*)data, RawDataLen);
          break;
        case 0x31:
          if (RawDataLen == sizeof(A25C31)) 
            memcpy((void*)& S25C31, (void*)data, RawDataLen);
          break;
        default:
          break;
        }
        break;
      default:
        break;
  }

  for (uint8_t i = 0; i < sizeof(_commandsWeWillSend); i++)
    if (AnswerHeader.cmd == pgm_read_byte_near(&_q[_commandsWeWillSend[i]])) {
      _NewDataFlag = 1;
      break;
    }
}

void prepareNextQuery() {
  static uint8_t index = 0;
  _Query._dynQueries[0] = 1;
  _Query._dynQueries[1] = 8;
  _Query._dynQueries[2] = 10;
  _Query._dynQueries[3] = 14;
  _Query._dynSize = 4;
  if (preloadQueryFromTable(_Query._dynQueries[index]) == 0) _Query.prepared = 1;
  index++;
  if (index >= _Query._dynSize) index = 0;
}

uint8_t preloadQueryFromTable(unsigned char index) {
  uint8_t* ptrBuf;
  uint8_t* pp;
  uint8_t* ph;
  uint8_t* pe;

  uint8_t cmdFormat;
  uint8_t hLen;
  uint8_t eLen;

  if (index >= sizeof(_q)) return 1;
  if (_Query.prepared != 0) return 2;
  cmdFormat = pgm_read_byte_near(_f + index);

  pp = (uint8_t*)&_h0;
  ph = NULL;
  pe = NULL;

  switch(cmdFormat) {
    case 1:
      ph = (uint8_t*)&_h1;
      hLen = sizeof(_h1);
      pe = NULL;
      break;
    case 2:
      ph = (uint8_t*)&_h2;
      hLen = sizeof(_h2);
      _end20t.hz = 0x02;
      _end20t.th = S20C00HZ65.throttle;
      _end20t.br = S20C00HZ65.brake;
      pe = (uint8_t*)&_end20t;
      eLen = sizeof(_end20t);
      break;
  }

  ptrBuf = (uint8_t*)&_Query.buf;
  memcpy_P((void*)ptrBuf, (void*)pp, sizeof(_h0));
  ptrBuf += sizeof(_h0);
  memcpy_P((void*)ptrBuf, (void*)ph, hLen);
  ptrBuf += hLen;
  memcpy_P((void*)ptrBuf, (void*)(_q + index), 1);
  ptrBuf++;
  memcpy_P((void*)ptrBuf, (void*)(_l + index), 1);
  ptrBuf++;
  if (pe != NULL) {
    memcpy((void*)ptrBuf, (void*)pe, eLen);
    ptrBuf+= eLen;
  }
  _Query.DataLen = ptrBuf - (uint8_t*)&_Query.buf[2];
  _Query.cs = calcCs((uint8_t*)&_Query.buf[2], _Query.DataLen);
  return 0;
}

void prepareCommand(uint8_t cmd) {
  uint8_t* ptrBuf;
  _cmd.len  = 4;
  _cmd.addr = 0x20;
  _cmd.rlen = 0x03;
  switch(cmd){
    case CMD_CRUISE_ON:  _cmd.param = 0x7C; _cmd.value = 1; break;
    case CMD_CRUISE_OFF: _cmd.param = 0x7C; _cmd.value = 0; break;
    case CMD_LED_ON:     _cmd.param = 0x7D; _cmd.value = 2; break;
    case CMD_LED_OFF:    _cmd.param = 0x7D; _cmd.value = 0; break;
    case CMD_WEAK:       _cmd.param = 0x7B; _cmd.value = 0; break;
    case CMD_MEDIUM:     _cmd.param = 0x7B; _cmd.value = 1; break;
    case CMD_STRONG:     _cmd.param = 0x7B; _cmd.value = 2; break;
    default: return;
  }
  ptrBuf = (uint8_t*)&_Query.buf;
  memcpy_P((void*)ptrBuf, (void*)_h0, sizeof(_h0));
  ptrBuf += sizeof(_h0);
  memcpy((void*)ptrBuf, (void*)&_cmd, sizeof(_cmd));
  ptrBuf += sizeof(_cmd);
  _Query.DataLen = ptrBuf - (uint8_t*)&_Query.buf[2];
  _Query.cs = calcCs((uint8_t*)&_Query.buf[2], _Query.DataLen);
  _Query.prepared = 1;
}

void writeQuery() {
  RX_DISABLE;
  XIAOMI_PORT.write((uint8_t*)&_Query.buf, _Query.DataLen + 2);
  XIAOMI_PORT.write((uint8_t*)&_Query.cs, 2);
  RX_ENABLE;
  _Query.prepared = 0;
}

uint16_t calcCs(uint8_t* data, uint8_t len) {
  uint16_t cs = 0xFFFF;
  for (uint8_t i = len; i > 0; i--) cs -= *data++;
  return cs;
}
