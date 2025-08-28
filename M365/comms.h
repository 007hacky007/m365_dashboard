#pragma once
#include "defines.h"

// RX FSM for incoming packets
void dataFSM();

// Packet processing
void processPacket(uint8_t* data, uint8_t len);

// Query preparation cycle
void prepareNextQuery();

// Build query from table
uint8_t preloadQueryFromTable(unsigned char index);

// Prepare and send commands
void prepareCommand(uint8_t cmd);
void writeQuery();

// Checksum helper
uint16_t calcCs(uint8_t* data, uint8_t len);
