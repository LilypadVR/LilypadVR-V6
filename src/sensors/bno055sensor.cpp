/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2021 Eiren Rain & SlimeVR contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
#include "bno055sensor.h"

#include <cstdint>

#include "GlobalVars.h"
#include "globals.h"

void BNO055Sensor::motionSetup() {
	imu = Adafruit_BNO055(sensorId, addr);
	delay(3000);
#if USE_6_AXIS
	if (!imu.begin(Adafruit_BNO055::OPERATION_MODE_IMUPLUS))
#else
	if (!imu.begin(Adafruit_BNO055::OPERATION_MODE_NDOF))
#endif
	{
		m_Logger.fatal("Can't connect to BNO055 at address 0x%02x", addr);
		ledManager.pattern(50, 50, 200);
		return;
	}

	delay(1000);
	imu.setExtCrystalUse(true);  // Adafruit BNO055's use external crystal. Enable it,
								 // otherwise it does not work.
	imu.setAxisRemap(Adafruit_BNO055::REMAP_CONFIG_P0);
	imu.setAxisSign(Adafruit_BNO055::REMAP_SIGN_P0);
	m_Logger.info("Connected to BNO055 at address 0x%02x", addr);

	working = true;
	m_tpsCounter.reset();
	m_dataCounter.reset();
}

void BNO055Sensor::motionLoop() {
	m_tpsCounter.update();
#if ENABLE_INSPECTION
	{
		Vector3 gyro = imu.getVector(Adafruit_BNO055::VECTOR_GYROSCOPE);
		Vector3 accel = imu.getVector(Adafruit_BNO055::VECTOR_LINEARACCEL);
		Vector3 mag = imu.getVector(Adafruit_BNO055::VECTOR_MAGNETOMETER);

		networkConnection.sendInspectionRawIMUData(
			sensorId,
			UNPACK_VECTOR(gyro),
			255,
			UNPACK_VECTOR(accel),
			255,
			UNPACK_VECTOR(mag),
			255
		);
	}
#endif

	uint8_t gyroCalibrationState;
	imu.getCalibration(nullptr, &gyroCalibrationState, nullptr, nullptr);
	if (gyroCalibrationState == 3) {
		markRestCalibrationComplete();
	}

	// TODO Optimize a bit with setting rawQuat directly
	setFusedRotation(imu.getQuat());
	hadData = true;

#if SEND_ACCELERATION
	setAcceleration(imu.getVector(Adafruit_BNO055::VECTOR_LINEARACCEL));
#endif
}

void BNO055Sensor::startCalibration(int calibrationType) {}

void BNO055Sensor::deinit() { imu.enterSuspendMode(); }
