/*
 * SCSCL.h
 * 飞特SCSCL系列串行舵机应用层程序
 * 日期: 2021.3.11
 * 作者:
 */

#ifndef _SCSCL_H
#define _SCSCL_H

//-------EPROM(只读)--------
#define SCSCL_VERSION_L 3
#define SCSCL_VERSION_H 4

//-------EPROM(读写)--------
#define SCSCL_ID 5
#define SCSCL_BAUD_RATE 6
#define SCSCL_MIN_ANGLE_LIMIT_L 9
#define SCSCL_MIN_ANGLE_LIMIT_H 10
#define SCSCL_MAX_ANGLE_LIMIT_L 11
#define SCSCL_MAX_ANGLE_LIMIT_H 12
#define SCSCL_CW_DEAD 26
#define SCSCL_CCW_DEAD 27

//-------SRAM(读写)--------
#define SCSCL_TORQUE_ENABLE 40
#define SCSCL_GOAL_POSITION_L 42
#define SCSCL_GOAL_POSITION_H 43
#define SCSCL_GOAL_TIME_L 44
#define SCSCL_GOAL_TIME_H 45
#define SCSCL_GOAL_SPEED_L 46
#define SCSCL_GOAL_SPEED_H 47
#define SCSCL_LOCK 48

//-------SRAM(只读)--------
#define SCSCL_PRESENT_POSITION_L 56
#define SCSCL_PRESENT_POSITION_H 57
#define SCSCL_PRESENT_SPEED_L 58
#define SCSCL_PRESENT_SPEED_H 59
#define SCSCL_PRESENT_LOAD_L 60
#define SCSCL_PRESENT_LOAD_H 61
#define SCSCL_PRESENT_VOLTAGE 62
#define SCSCL_PRESENT_TEMPERATURE 63
#define SCSCL_MOVING 66
#define SCSCL_PRESENT_CURRENT_L 69
#define SCSCL_PRESENT_CURRENT_H 70

#include "SCSerial.h"

class SCSCL : public SCSerial
{
public:
	SCSCL();
	SCSCL(u8 End);
	SCSCL(u8 End, u8 Level);

	void begin(const struct device *uart_dev);

	virtual int WritePos(u8 ID, u16 Position, u16 Time, u16 Speed);
	virtual int WritePosEx(u8 ID, s16 Position, u16 Speed, u8 ACC);
	virtual int RegWritePos(u8 ID, u16 Position, u16 Time, u16 Speed = 0);
	virtual void SyncWritePos(u8 ID[], u8 IDN, u16 Position[], u16 Time[], u16 Speed[]);
	virtual int PWMMode(u8 ID);
	virtual int WritePWM(u8 ID, s16 pwmOut);
	virtual int EnableTorque(u8 ID, u8 Enable);
	virtual int unLockEprom(u8 ID);
	virtual int LockEprom(u8 ID);
	virtual int FeedBack(int ID);
	virtual int ReadPos(int ID);
	virtual int ReadSpeed(int ID);
	virtual int ReadLoad(int ID);
	virtual int ReadVoltage(int ID);
	virtual int ReadTemper(int ID);
	virtual int ReadMove(int ID);
	virtual int ReadCurrent(int ID);
	virtual int ReadMode(int ID);
	virtual int CalibrationOfs(u8 ID);
	virtual int ReadInfoValue(int ID, int AddInput);
static constexpr size_t MEM_SIZE = SCSCL_PRESENT_CURRENT_H - SCSCL_PRESENT_POSITION_L + 1;
private:
	u8 Mem[MEM_SIZE];
};

#endif