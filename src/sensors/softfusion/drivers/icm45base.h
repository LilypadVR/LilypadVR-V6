/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2024 Gorbit99 & SlimeVR Contributors
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

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "../../../sensorinterface/RegisterInterface.h"

namespace SlimeVR::Sensors::SoftFusion::Drivers {

// Driver uses acceleration range at 32g
// and gyroscope range at 4000dps
// using high resolution mode
// Uses 32.768kHz clock
// Gyroscope ODR = 409.6Hz, accel ODR = 102.4Hz
// Timestamps reading not used, as they're useless (constant predefined increment)

struct ICM45Base {
	static constexpr uint8_t Address = 0x68;

	static constexpr float GyrTs = 1.0 / 409.6;
	static constexpr float AccTs = 1.0 / 102.4;
	static constexpr float TempTs = 1.0 / 409.6;

	static constexpr float MagTs = 1.0 / 100;

	static constexpr float GyroSensitivity = 131.072f;
	static constexpr float AccelSensitivity = 16384.0f;

	static constexpr float TemperatureBias = 25.0f;
	static constexpr float TemperatureSensitivity = 128.0f;

	static constexpr float TemperatureZROChange = 20.0f;

	static constexpr bool Uses32BitSensorData = true;

	RegisterInterface& m_RegisterInterface;
	SlimeVR::Logging::Logger& m_Logger;
	ICM45Base(RegisterInterface& registerInterface, SlimeVR::Logging::Logger& logger)
		: m_RegisterInterface(registerInterface)
		, m_Logger(logger) {}

	struct BaseRegs {
		static constexpr uint8_t TempData = 0x0c;

		struct DeviceConfig {
			static constexpr uint8_t reg = 0x7f;
			static constexpr uint8_t valueSwReset = 0b11;
		};

		struct GyroConfig {
			static constexpr uint8_t reg = 0x1c;
			static constexpr uint8_t value
				= (0b0000 << 4) | 0b0111;  // 4000dps, odr=409.6Hz
		};

		struct AccelConfig {
			static constexpr uint8_t reg = 0x1b;
			static constexpr uint8_t value
				= (0b000 << 4) | 0b1001;  // 32g, odr = 102.4Hz
		};

		struct FifoConfig0 {
			static constexpr uint8_t reg = 0x1d;
			static constexpr uint8_t value
				= (0b01 << 6) | (0b011111);  // stream to FIFO mode, FIFO depth
											 // 8k bytes <-- this disables all APEX
											 // features, but we don't need them
		};

		struct FifoConfig3 {
			static constexpr uint8_t reg = 0x21;
			static constexpr uint8_t value = (0b1 << 0) | (0b1 << 1) | (0b1 << 2)
										   | (0b1 << 3);  // enable FIFO,
														  // enable accel,
														  // enable gyro,
														  // enable hires mode
		};

		struct PwrMgmt0 {
			static constexpr uint8_t reg = 0x10;
			static constexpr uint8_t value
				= 0b11 | (0b11 << 2);  // accel in low noise mode, gyro in low noise
		};

		static constexpr uint8_t FifoCount = 0x12;
		static constexpr uint8_t FifoData = 0x14;
	};

#pragma pack(push, 1)
	struct FifoEntryAligned {
		int16_t accel[3];
		int16_t gyro[3];
		uint16_t temp;
		uint16_t timestamp;
		uint8_t lsb[3];
	};
#pragma pack(pop)

	static constexpr size_t FullFifoEntrySize = sizeof(FifoEntryAligned) + 1;

	void softResetIMU() {
		m_RegisterInterface.writeReg(
			BaseRegs::DeviceConfig::reg,
			BaseRegs::DeviceConfig::valueSwReset
		);
		delay(35);
	}

	bool initializeBase() {
		// perform initialization step
		m_RegisterInterface.writeReg(
			BaseRegs::GyroConfig::reg,
			BaseRegs::GyroConfig::value
		);
		m_RegisterInterface.writeReg(
			BaseRegs::AccelConfig::reg,
			BaseRegs::AccelConfig::value
		);
		m_RegisterInterface.writeReg(
			BaseRegs::FifoConfig0::reg,
			BaseRegs::FifoConfig0::value
		);
		m_RegisterInterface.writeReg(
			BaseRegs::FifoConfig3::reg,
			BaseRegs::FifoConfig3::value
		);
		m_RegisterInterface.writeReg(
			BaseRegs::PwrMgmt0::reg,
			BaseRegs::PwrMgmt0::value
		);
		delay(1);

		return true;
	}

	template <typename AccelCall, typename GyroCall, typename TempCall>
	void bulkRead(
		AccelCall&& processAccelSample,
		GyroCall&& processGyroSample,
		TempCall&& processTemperatureSample
	) {
		// Allocate statically so that it does not take up stack space, which
		// can result in stack overflow and panic
		constexpr size_t MaxReadings = 8;
		static std::array<uint8_t, FullFifoEntrySize * MaxReadings> read_buffer;

		constexpr int16_t InvalidReading = -32768;

		size_t fifo_packets = m_RegisterInterface.readReg16(BaseRegs::FifoCount);

		if (fifo_packets >= 1) {
			//
			// AN-000364
			// 2.16 FIFO EMPTY EVENT IN STREAMING MODE CAN CORRUPT FIFO DATA
			//
			// Description: When in FIFO streaming mode, a FIFO empty event
			// (caused by host reading the last byte of the last FIFO frame) can
			// cause FIFO data corruption in the first FIFO frame that arrives
			// after the FIFO empty condition. Once the issue is triggered, the
			// FIFO state is compromised and cannot recover. FIFO must be set in
			// bypass mode to flush out the wrong state
			//
			// When operating in FIFO streaming mode, if FIFO threshold
			// interrupt is triggered with M number of FIFO frames accumulated
			// in the FIFO buffer, the host should only read the first M-1
			// number of FIFO frames. This prevents the FIFO empty event, that
			// can cause FIFO data corruption, from happening.
			//
			--fifo_packets;
		}

		if (fifo_packets == 0) {
			return;
		}

		fifo_packets = std::min(fifo_packets, MaxReadings);

		size_t bytes_to_read = fifo_packets * FullFifoEntrySize;
		m_RegisterInterface
			.readBytes(BaseRegs::FifoData, bytes_to_read, read_buffer.data());

		for (auto i = 0u; i < bytes_to_read; i += FullFifoEntrySize) {
			uint8_t header = read_buffer[i];
			bool has_gyro = header & (1 << 5);
			bool has_accel = header & (1 << 6);

			FifoEntryAligned entry;
			memcpy(
				&entry,
				&read_buffer[i + 0x1],
				sizeof(FifoEntryAligned)
			);  // skip fifo header

			if (has_gyro && entry.gyro[0] != InvalidReading) {
				const int32_t gyroData[3]{
					static_cast<int32_t>(entry.gyro[0]) << 4 | (entry.lsb[0] & 0xf),
					static_cast<int32_t>(entry.gyro[1]) << 4 | (entry.lsb[1] & 0xf),
					static_cast<int32_t>(entry.gyro[2]) << 4 | (entry.lsb[2] & 0xf),
				};
				processGyroSample(gyroData, GyrTs);
			}

			if (has_accel && entry.accel[0] != InvalidReading) {
				const int32_t accelData[3]{
					static_cast<int32_t>(entry.accel[0]) << 4
						| (static_cast<int32_t>((entry.lsb[0]) & 0xf0) >> 4),
					static_cast<int32_t>(entry.accel[1]) << 4
						| (static_cast<int32_t>((entry.lsb[1]) & 0xf0) >> 4),
					static_cast<int32_t>(entry.accel[2]) << 4
						| (static_cast<int32_t>((entry.lsb[2]) & 0xf0) >> 4),
				};
				processAccelSample(accelData, AccTs);
			}

			if (entry.temp != 0x8000) {
				processTemperatureSample(static_cast<int16_t>(entry.temp), TempTs);
			}
		}
	}

	void deinit() { softResetIMU(); }
};

};  // namespace SlimeVR::Sensors::SoftFusion::Drivers
