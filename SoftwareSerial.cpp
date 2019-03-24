/*

SoftwareSerial.cpp - Implementation of the Arduino software serial for ESP8266/ESP32.
Copyright (c) 2015-2016 Peter Lerup. All rights reserved.
Copyright (c) 2018-2019 Dirk O. Kaar. All rights reserved.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#include <Arduino.h>

#include <SoftwareSerial.h>

// signal quality in ALT_DIGITAL_WRITE is better or equal in all
// tests so far (ESP8266 HW UART, SDS011 PM sensor, SoftwareSerial back-to-back).
#define ALT_DIGITAL_WRITE 1

// Modify MAX_SWS_INSTS and comment out sws_isr_* trampolines and adapt the
// ISRList initializer accordingly if reducing the footprint of these
// data structures is desired.
#if defined(ESP8266)
constexpr size_t MAX_SWS_INSTS = 10;
#elif defined(ESP32)
constexpr size_t MAX_SWS_INSTS = 22;
#endif

// As the ESP8266 Arduino attachInterrupt has no parameter, lists of objects
// and callbacks corresponding to each possible list index have to be defined
static ICACHE_RAM_ATTR SoftwareSerial* ObjList[MAX_SWS_INSTS];

template<int I> void ICACHE_RAM_ATTR sws_isr() {
	ObjList[I]->rxRead();
}

static void (* const ISRList[MAX_SWS_INSTS])() = {
	sws_isr<0>,
	sws_isr<1>,
	sws_isr<2>,
	sws_isr<3>,
	sws_isr<4>,
	sws_isr<5>,
	sws_isr<6>,
	sws_isr<7>,
	sws_isr<8>,
	sws_isr<9>,
#ifdef ESP32
	sws_isr<10>,
	sws_isr<11>,
	sws_isr<12>,
	sws_isr<13>,
	sws_isr<14>,
	sws_isr<15>,
	sws_isr<16>,
	sws_isr<17>,
	sws_isr<18>,
	sws_isr<19>,
	sws_isr<20>,
	sws_isr<21>,
#endif
};

SoftwareSerial::SoftwareSerial(
	int receivePin, int transmitPin, bool inverse_logic, int bufSize, int isrBufSize) {
	m_isrBuffer = 0;
	m_isrOverflow = false;
	m_isrLastCycle = 0;
	m_oneWire = (receivePin == transmitPin);
	m_invert = inverse_logic;
	if (isValidGPIOpin(receivePin)) {
		m_rxPin = receivePin;
		m_bufSize = bufSize;
		m_buffer = (uint8_t*)malloc(m_bufSize);
		m_isrBufSize = isrBufSize ? isrBufSize : 10 * bufSize;
		m_isrBuffer = static_cast<std::atomic<uint32_t>*>(malloc(m_isrBufSize * sizeof(uint32_t)));
	}
	if (isValidGPIOpin(transmitPin) || (!m_oneWire && (transmitPin == 16))) {
		m_txValid = true;
		m_txPin = transmitPin;
	}
}

SoftwareSerial::~SoftwareSerial() {
	end();
	if (m_buffer) {
		free(m_buffer);
	}
	if (m_isrBuffer) {
		free(m_isrBuffer);
	}
}

bool SoftwareSerial::isValidGPIOpin(int pin) {
#ifdef ESP8266
	return (pin >= 0 && pin <= 5) || (pin >= 12 && pin <= 15);
#endif
#ifdef ESP32
	return pin == 0 || pin == 2 || (pin >= 4 && pin <= 5) || (pin >= 12 && pin <= 19) ||
		(pin >= 21 && pin <= 23) || (pin >= 25 && pin <= 27) || (pin >= 32 && pin <= 35);
#endif
}

bool SoftwareSerial::begin(int32_t baud) {
	if (m_swsInstsIdx < 0)
		for (size_t i = 0; i < MAX_SWS_INSTS; ++i)
		{
			if (!ObjList[i]) {
				m_swsInstsIdx = i;
				ObjList[m_swsInstsIdx] = this;
				break;
			}
		}
	if (m_swsInstsIdx < 0) return false;
	m_bitCycles = ESP.getCpuFreqMHz() * 1000000 / baud;
	m_intTxEnabled = true;
	if (m_buffer != 0 && m_isrBuffer != 0) {
		m_rxValid = true;
		m_inPos = m_outPos = 0;
		m_isrInPos.store(0);
		m_isrOutPos.store(0);
		pinMode(m_rxPin, INPUT_PULLUP);
	}
	if (m_txValid && !m_oneWire) {
#ifdef ALT_DIGITAL_WRITE
		digitalWrite(m_txPin, LOW);
		pinMode(m_txPin, m_invert ? OUTPUT : INPUT_PULLUP);
#else
		pinMode(m_txPin, OUTPUT);
		digitalWrite(m_txPin, !m_invert);
#endif
	}

	if (!m_rxEnabled) { enableRx(true); }
	return true;
}

void SoftwareSerial::end()
{
	enableRx(false);
	if (m_swsInstsIdx >= 0)	{
		ObjList[m_swsInstsIdx] = 0;
		m_swsInstsIdx = -1;
	}
}

int32_t SoftwareSerial::baudRate() {
	return ESP.getCpuFreqMHz() * 1000000 / m_bitCycles;
}

void SoftwareSerial::setTransmitEnablePin(int transmitEnablePin) {
	if (isValidGPIOpin(transmitEnablePin)) {
		m_txEnableValid = true;
		m_txEnablePin = transmitEnablePin;
#ifdef ALT_DIGITAL_WRITE
		digitalWrite(m_txEnablePin, LOW);
		pinMode(m_txEnablePin, OUTPUT);
#else
		pinMode(m_txEnablePin, OUTPUT);
		digitalWrite(m_txEnablePin, LOW);
#endif
	} else {
		m_txEnableValid = false;
	}
}

void SoftwareSerial::enableIntTx(bool on) {
	m_intTxEnabled = on;
}

void SoftwareSerial::enableTx(bool on) {
	if (m_oneWire && m_txValid) {
		if (on) {
			enableRx(false);
#ifdef ALT_DIGITAL_WRITE
			digitalWrite(m_txPin, LOW);
			pinMode(m_txPin, m_invert ? OUTPUT : INPUT_PULLUP);
			digitalWrite(m_rxPin, LOW);
			pinMode(m_rxPin, m_invert ? OUTPUT : INPUT_PULLUP);
#else
			pinMode(m_txPin, OUTPUT);
			digitalWrite(m_txPin, !m_invert);
			pinMode(m_rxPin, OUTPUT);
			digitalWrite(m_rxPin, !m_invert);
#endif
		} else {
#ifdef ALT_DIGITAL_WRITE
			digitalWrite(m_txPin, LOW);
			pinMode(m_txPin, m_invert ? OUTPUT : INPUT_PULLUP);
#else
			pinMode(m_txPin, OUTPUT);
			digitalWrite(m_txPin, !m_invert);
#endif
			pinMode(m_rxPin, INPUT_PULLUP);
			enableRx(true);
		}
	}
}

void SoftwareSerial::enableRx(bool on) {
	if (m_rxValid) {
		if (on) {
			m_rxCurBit = 8;
			attachInterrupt(digitalPinToInterrupt(m_rxPin), ISRList[m_swsInstsIdx], CHANGE);
		} else {
			detachInterrupt(digitalPinToInterrupt(m_rxPin));
		}
		m_rxEnabled = on;
	}
}

int SoftwareSerial::read() {
	if (!m_rxValid) { return -1; }
	if (m_inPos == m_outPos) {
		rxBits();
		if (m_inPos == m_outPos) { return -1; }
	}
	uint8_t ch = m_buffer[m_outPos];
	m_outPos = (m_outPos + 1) % m_bufSize;
	return ch;
}

int SoftwareSerial::available() {
	if (!m_rxValid) { return 0; }
	rxBits();
	int avail = m_inPos - m_outPos;
	if (avail < 0) { avail += m_bufSize; }
	if (!avail) {
		optimistic_yield(20 * m_bitCycles / ESP.getCpuFreqMHz());
		rxBits();
		avail = m_inPos - m_outPos;
		if (avail < 0) { avail += m_bufSize; }
	}
	return avail;
}

void ICACHE_RAM_ATTR SoftwareSerial::preciseDelay(uint32_t deadline) {
	int32_t micro_s = static_cast<int32_t>(deadline - ESP.getCycleCount()) / ESP.getCpuFreqMHz();
	// Reenable interrupts while delaying to avoid other tasks piling up
	if (!m_intTxEnabled) { interrupts(); }
	if (micro_s > 1) {
		delayMicroseconds(micro_s - 1);
	}
	// Disable interrupts again
	if (!m_intTxEnabled) { noInterrupts(); }
	while (static_cast<int32_t>(deadline - ESP.getCycleCount()) > 1) {}
}

void ICACHE_RAM_ATTR SoftwareSerial::writePeriod(uint32_t dutyCycle, uint32_t offCycle) {
	if (dutyCycle) {
		m_periodDeadline += dutyCycle;
#ifdef ALT_DIGITAL_WRITE
		pinMode(m_txPin, INPUT_PULLUP);
#else
		digitalWrite(m_txPin, HIGH);
#endif
		preciseDelay(m_periodDeadline);
	}
	if (offCycle) {
		m_periodDeadline += offCycle;
#ifdef ALT_DIGITAL_WRITE
		pinMode(m_txPin, OUTPUT);
#else
		digitalWrite(m_txPin, LOW);
#endif
		preciseDelay(m_periodDeadline);
	}
}

size_t ICACHE_RAM_ATTR SoftwareSerial::write(uint8_t b) {
	return write(&b, 1);
}

size_t ICACHE_RAM_ATTR SoftwareSerial::write(const uint8_t *buffer, size_t size) {
	if (m_rxValid) { rxBits(); }
	if (!m_txValid) { return 0; }

	if (m_txEnableValid) {
#ifdef ALT_DIGITAL_WRITE
		pinMode(m_txEnablePin, INPUT_PULLUP);
#else
		digitalWrite(m_txEnablePin, HIGH);
#endif
	}
	// Stop bit level : LOW if inverted logic, otherwise HIGH
#ifdef ALT_DIGITAL_WRITE
	pinMode(m_txPin, m_invert ? OUTPUT : INPUT_PULLUP);
#else
	digitalWrite(m_txPin, !m_invert);
#endif
	uint32_t dutyCycle = 0;
	uint32_t offCycle = 0;
	bool pb;
	// Disable interrupts in order to get a clean transmit timing
	if (!m_intTxEnabled) { noInterrupts(); }
	m_periodDeadline = ESP.getCycleCount();
	for (int cnt = 0; cnt < size; ++cnt, ++buffer) {
		// Start bit : HIGH if inverted logic, otherwise LOW
		if (m_invert) { dutyCycle += m_bitCycles; } else { offCycle += m_bitCycles; }
		pb = m_invert;
		uint8_t o = m_invert ? ~*buffer : *buffer;
		bool b;
		for (int i = 0; i < 9; ++i) {
			// data bit
			// or stop bit : LOW if inverted logic, otherwise HIGH
			b = (i < 8) ? (o & 1) : !m_invert;
			o >>= 1;
			if (!pb && b) {
				writePeriod(dutyCycle, offCycle);
				dutyCycle = offCycle = 0;
			}
			if (b) { dutyCycle += m_bitCycles; } else { offCycle += m_bitCycles; }
			pb = b;
		}
		if (cnt == size - 1) {
			writePeriod(dutyCycle, offCycle);
			break;
		}
	}
	if (!m_intTxEnabled) { interrupts(); }
	if (m_txEnableValid) {
#ifdef ALT_DIGITAL_WRITE
		pinMode(m_txEnablePin, OUTPUT);
#else
		digitalWrite(m_txEnablePin, LOW);
#endif
	}
	return size;
}

void SoftwareSerial::flush() {
	m_inPos = m_outPos = 0;
	m_isrInPos.store(0);
	m_isrOutPos.store(0);
}

bool SoftwareSerial::overflow() {
	bool res = m_overflow;
	m_overflow = false;
	return res;
}

int SoftwareSerial::peek() {
	if (!m_rxValid || (rxBits(), m_inPos == m_outPos)) { return -1; }
	return m_buffer[m_outPos];
}

void ICACHE_RAM_ATTR SoftwareSerial::rxBits() {
	int avail = m_isrInPos.load() - m_isrOutPos.load();
	if (avail < 0) { avail += m_isrBufSize; }
	if (m_isrOverflow.load()) {
		m_overflow = true;
		m_isrOverflow.store(false);
	}

	// stop bit can go undetected if leading data bits are at same level
	// and there was also no next start bit yet, so one byte may be pending.
	// low-cost check first
	if (avail == 0 && m_rxCurBit < 8 && m_isrInPos.load() == m_isrOutPos.load() && m_rxCurBit >= 0) {
		uint32_t delta = ESP.getCycleCount() - m_isrLastCycle.load();
		uint32_t expectedDelta = (10 - m_rxCurBit) * m_bitCycles;
		if (delta >= expectedDelta) {
			// Store inverted stop bit edge and cycle in the buffer unless we have an overflow
			// cycle's LSB is repurposed for the level bit
			int next = (m_isrInPos.load() + 1) % m_isrBufSize;
			if (next != m_isrOutPos.load()) {
				uint32_t expectedCycle = m_isrLastCycle.load() + expectedDelta;
				m_isrBuffer[m_isrInPos.load()].store((expectedCycle | 1) ^ !m_invert);
				m_isrInPos.store(next);
				++avail;
			} else {
				m_isrOverflow.store(true);
			}
		}
	}

	while (avail--) {
		// error introduced by edge value in LSB is neglegible
		uint32_t isrCycle = m_isrBuffer[m_isrOutPos.load()].load();
		// extract inverted edge value
		bool level = (isrCycle & 1) == m_invert;
		m_isrOutPos.store((m_isrOutPos.load() + 1) % m_isrBufSize);
		int32_t cycles = static_cast<int32_t>(isrCycle - m_isrLastCycle.load()) - (m_bitCycles / 2);
		if (cycles < 0) { continue; }
		m_isrLastCycle.store(isrCycle);
		do {
			// data bits
			if (m_rxCurBit >= -1 && m_rxCurBit < 7) {
				if (cycles >= m_bitCycles) {
					// preceding masked bits
					int hiddenBits = cycles / m_bitCycles;
					if (hiddenBits > 7 - m_rxCurBit) { hiddenBits = 7 - m_rxCurBit; }
					bool lastBit = m_rxCurByte & 0x80;
					m_rxCurByte >>= hiddenBits;
					// masked bits have same level as last unmasked bit
					if (lastBit) { m_rxCurByte |= 0xff << (8 - hiddenBits); }
					m_rxCurBit += hiddenBits;
					cycles -= hiddenBits * m_bitCycles;
				}
				if (m_rxCurBit < 7) {
					++m_rxCurBit;
					cycles -= m_bitCycles;
					m_rxCurByte >>= 1;
					if (level) { m_rxCurByte |= 0x80; }
				}
				continue;
			}
			if (m_rxCurBit == 7) {
				m_rxCurBit = 8;
				cycles -= m_bitCycles;
				// Store the received value in the buffer unless we have an overflow
				int next = (m_inPos + 1) % m_bufSize;
				if (next != m_outPos) {
					m_buffer[m_inPos] = m_rxCurByte;
					// reset to 0 is important for masked bit logic
					m_rxCurByte = 0;
					m_inPos = next;
				} else {
					m_overflow = true;
				}
				continue;
			}
			if (m_rxCurBit == 8) {
				// start bit level is low
				if (!level) {
					m_rxCurBit = -1;
				}
			}
			break;
		} while (cycles >= 0);
	}
}

void ICACHE_RAM_ATTR SoftwareSerial::rxRead() {
	uint32_t curCycle = ESP.getCycleCount();
	bool level = digitalRead(m_rxPin);

	// Store inverted edge value & cycle in the buffer unless we have an overflow
	// cycle's LSB is repurposed for the level bit
	int next = (m_isrInPos.load() + 1) % m_isrBufSize;
	if (next != m_isrOutPos.load()) {
		m_isrBuffer[m_isrInPos.load()].store((curCycle | 1) ^ level);
		m_isrInPos.store(next);
	} else {
		m_isrOverflow.store(true);
	}
}

void SoftwareSerial::onReceive(std::function<void(int available)> handler) {
	receiveHandler = handler;
}

void SoftwareSerial::perform_work() {
	if (receiveHandler) {
		if (!m_rxValid) { return; }
		rxBits();
		int avail = m_inPos - m_outPos;
		if (avail < 0) { avail += m_bufSize; }
		if (avail) { receiveHandler(avail); }
	}
}
