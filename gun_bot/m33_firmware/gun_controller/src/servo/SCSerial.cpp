/*
 * SCSerial.cpp
 * Zephyr UART implementation for SCServo
 */

#include "SCSerial.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

SCSerial::SCSerial() : SCS()
{
	IOTimeOut = 100;
	Err = 0;
}

SCSerial::SCSerial(u8 End) : SCS(End)
{
	IOTimeOut = 100;
	Err = 0;
}

SCSerial::SCSerial(u8 End, u8 Level) : SCS(End, Level)
{
	IOTimeOut = 100;
	Err = 0;
}

void SCSerial::begin(const struct device *uart_dev)
{
	SCS::uart_dev = uart_dev;
	Err = 0;
}

int SCSerial::readSCS(unsigned char *nDat, int nLen)
{
	if (!SCS::uart_dev) {
		Err = 1;
		return -1;
	}

	int Size = 0;
	uint64_t start = k_uptime_get();

	while (Size < nLen) {
		int c = -1;
		uint8_t byte;

		if (uart_poll_in(SCS::uart_dev, &byte) == 0) {
			c = byte;
		}

		if (c != -1) {
			if (nDat) {
				nDat[Size] = c;
			}
			Size++;
			start = k_uptime_get();
		} else {
			// Check timeout
			if (k_uptime_get() - start > IOTimeOut) {
				break;
			}
			// Small delay to avoid busy loop
			k_sleep(K_MSEC(1));
		}
	}

	if (Size < nLen) {
		Err = 1;
	}
	return Size;
}

int SCSerial::writeSCS(unsigned char *nDat, int nLen)
{
	if (!SCS::uart_dev) {
		return 0;
	}

	if (nDat == nullptr) {
		return 0;
	}

	int written = 0;
	for (int i = 0; i < nLen; i++) {
		uart_poll_out(SCS::uart_dev, nDat[i]);
		written++;
	}
	return written;
}

int SCSerial::writeSCS(unsigned char bDat)
{
	return writeSCS(&bDat, 1);
}

void SCSerial::rFlushSCS()
{
	uint8_t c;
	while (uart_poll_in(SCS::uart_dev, &c) == 0) {
		// discard
	}
}

void SCSerial::wFlushSCS()
{
	// UART is同步的, no separate write buffer
}