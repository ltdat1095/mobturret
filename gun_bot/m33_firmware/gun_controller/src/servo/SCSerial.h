/*
 * SCSerial.h
 * Zephyr UART implementation for SCServo
 * 日期: 2019.4.27
 * 作者:
 */

#ifndef _SCSERIAL_H
#define _SCSERIAL_H

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include "SCS.h"

class SCSerial : public SCS
{
public:
	SCSerial();
	SCSerial(u8 End);
	SCSerial(u8 End, u8 Level);

	void begin(const struct device *uart_dev);

protected:
	virtual int writeSCS(unsigned char *nDat, int nLen) override;
	virtual int readSCS(unsigned char *nDat, int nLen) override;
	virtual int writeSCS(unsigned char bDat) override;
	virtual void rFlushSCS() override;
	virtual void wFlushSCS() override;

public:
	int Err;
};

#endif