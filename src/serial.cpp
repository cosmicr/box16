// Commander X16 Emulator
// Copyright (c) 2022 Michael Steil
// All rights reserved. License: 2-clause BSD

// Commodore Bus emulation, L1: Serial Bus
// This code is hacky, buggy and incomplete, but
// it helped bringing up the serial bus on real
// hardware.

#include "serial.h"
#include "glue.h"
#include "ieee.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

serial_port_t serial_port;

static int     state = 0;
static bool    valid;
static int     bit;
static uint8_t serial_byte;
static bool    listening                = false;
static bool    talking                  = false;
static bool    during_atn               = false;
static bool    eoi                      = false;
static bool    fnf                      = false; // file not found
static int     clocks_since_last_change = 0;

#define LOG_SERIAL(...) // fmt::print(__VA_ARGS__)

static uint8_t read_serial_byte(bool *eoi)
{
	uint8_t serial_byte;
	int     ret = ACPTR(&serial_byte);
	*eoi        = ret >= 0;
	return serial_byte;
}

int serial_port_read_clk()
{
	return serial_port.out.clk & serial_port.in.clk;
}

int serial_port_read_data()
{
	return serial_port.out.data & serial_port.in.data;
}

void serial_step(int clocks)
{
	bool print = false;

	static int old_atn = 0;
	static int old_clk = 0;
	static int old_data = 0;
	if (old_atn == serial_port.in.atn &&
	    old_clk == serial_port_read_clk() &&
	    old_data == serial_port_read_data()) {
		clocks_since_last_change += clocks;
		if (state == 2 && valid == true && bit == 0 && clocks_since_last_change > 200 * MHZ) {
			if (clocks_since_last_change < (200 + 60) * MHZ) {
				LOG_SERIAL("XXX EOI ACK\n");
				serial_port.out.data = 0;
				eoi                  = true;
			} else {
				LOG_SERIAL("XXX EOI ACK END\n");
				serial_port.out.data     = 1;
				clocks_since_last_change = 0;
			}
			print = true;
		}
		if (state == 10 && clocks_since_last_change > 60 * MHZ) {
			serial_port.out.clk      = 1;
			state                    = 11;
			clocks_since_last_change = 0;
			print                    = true;
		} else if (state == 11 && serial_port_read_data() && !fnf) {
			clocks_since_last_change = 0;
			serial_byte              = read_serial_byte(&eoi);
			bit                      = 0;
			valid                    = true;
			if (eoi) {
				LOG_SERIAL("XXXEOI1\n");
				state = 12;
			} else {
				LOG_SERIAL("XXXEOI0\n");
				serial_port.out.clk = 0;
				state               = 13;
			}
			print = true;
		} else if (state == 12 && clocks_since_last_change > 512 * MHZ) {
			// EOI delay
			// XXX we'd have to check for the ACK
			clocks_since_last_change = 0;
			serial_port.out.clk      = 0;
			state                    = 13;
			print                    = true;
		} else if (state == 13 && clocks_since_last_change > 60 * MHZ) {
			if (valid) {
				// send bit
				serial_port.out.data = (serial_byte >> bit) & 1;
				serial_port.out.clk  = 1;
				LOG_SERIAL("*** BIT{:d} OUT: {:d}\n", bit, serial_port.out.data);
				bit++;
				if (bit == 8) {
					state = 14;
				}
			} else {
				serial_port.out.clk = 0;
			}
			valid                    = !valid;
			clocks_since_last_change = 0;
			print                    = true;
		} else if (state == 14 && clocks_since_last_change > 60 * MHZ) {
			serial_port.out.data     = 1;
			serial_port.out.clk      = 0;
			state                    = 10;
			clocks_since_last_change = 0;
			print                    = true;
		}
	} else {
		clocks_since_last_change = 0;

		LOG_SERIAL("-SERIAL IN {{ ATN:{:d} CLK:{:d} DATA:{:d} }}\n", serial_port.in.atn, old_clk, old_data);
		LOG_SERIAL("+SERIAL IN {{ ATN:{:d} CLK:{:d} DATA:{:d} }} --- IN {{ CLK:{:d} DATA:{:d} }} OUT {{ CLK:{:d} DATA:{:d} }} -- #{:d}\n", serial_port.in.atn, serial_port_read_clk(), serial_port_read_data(), serial_port.in.clk, serial_port.in.data, serial_port.out.clk, serial_port.out.data, state);

		if (!during_atn && serial_port.in.atn) {
			serial_port.out.data = 0;
			state                = 99;
			during_atn           = true;
			LOG_SERIAL("XXX START OF ATN\n");
		}

		switch (state) {
			case 0:
				break;
			case 99:
				// wait for CLK=0
				if (!serial_port_read_clk()) {
					state = 1;
				}
				break;
			case 1:
				if (during_atn && !serial_port.in.atn) {
					// cancelled ATN
					serial_port.out.data = 1;
					serial_port.out.clk  = 1;
					during_atn           = false;
					LOG_SERIAL("*** END OF ATN\n");
					if (listening) {
						// keep holding DATA to indicate we're here
						serial_port.out.data = 0;
						LOG_SERIAL("XXX START OF DATA RECEIVE\n");
					} else if (talking) {
						serial_port.out.clk = 0;
						state               = 10;
						LOG_SERIAL("XXX START OF DATA SEND\n");
					} else {
						state = 0;
					}
					break;
				}
				// wait for CLK=1
				if (serial_port_read_clk()) {
					serial_port.out.data = 1;
					state                = 2;
					valid                = true;
					bit                  = 0;
					serial_byte          = 0;
					eoi                  = false;
					LOG_SERIAL("XXX START OF serial_byte\n");
				}
				break;
			case 2:
				if (during_atn && !serial_port.in.atn) {
					// cancelled ATN
					serial_port.out.data = 1;
					serial_port.out.clk  = 1;
					LOG_SERIAL("*** XEND OF ATN\n");
					state = 0;
					break;
				}
				if (valid) {
					// wait for CLK=0, data not valid
					if (!serial_port_read_clk()) {
						valid = false;
						LOG_SERIAL("XXX NOT VALID\n");
					}
				} else {
					// wait for CLK=1, data valid
					if (serial_port_read_clk()) {
						bool b = serial_port_read_data();
						serial_byte |= (b << bit);
						LOG_SERIAL("*** BIT{:d} IN: {:d}\n", bit, b);
						valid = true;
						if (++bit == 8) {
							LOG_SERIAL("*** {} serial_byte IN: {:02x}{}\n", during_atn ? "ATN" : "DATA", serial_byte, eoi ? " (EOI)" : "");
							if (during_atn) {
								LOG_SERIAL("IEEE CMD {:x}\n", serial_byte);
								switch (serial_byte & 0x60) {
									case 0x20:
										if (serial_byte == 0x3f) {
											int ret   = UNLSN();
											fnf       = ret == 2;
											listening = false;
										} else {
											LISTEN(serial_byte);
											listening = true;
										}
										break;
									case 0x40:
										if (serial_byte == 0x5f) {
											UNTLK();
											talking = false;
										} else {
											TALK(serial_byte);
											talking = true;
										}
										break;
									case 0x60:
										if (listening) {
											SECOND(serial_byte);
										} else { // talking
											TKSA(serial_byte);
										}
										break;
								}
							} else {
								CIOUT(serial_byte);
							}
							serial_port.out.data = 0;
							state                = 1;
						}
					}
				}
				break;
			case 3:
				break;
		}
		print = true;
	}

	if (print) {
		LOG_SERIAL(">SERIAL IN {{ ATN:{:d} CLK:{:d} DATA:{:d} }} --- IN {{ CLK:{:d} DATA:{:d} }} OUT {{ CLK:{:d} DATA:{:d} }} -- #{:d}\n", serial_port.in.atn, serial_port_read_clk(), serial_port_read_data(), serial_port.in.clk, serial_port.in.data, serial_port.out.clk, serial_port.out.data, state);
	}

	old_atn  = serial_port.in.atn;
	old_clk  = serial_port_read_clk();
	old_data = serial_port_read_data();
}
