/**
 * Copyright (C) 2026 MK124 and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* StopWatch board assembly and the desktop shared-memory connection. */

#include "qemu/osdep.h"
#include "m5_stopwatch.h"

#include "hw/qdev-properties.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "sysemu/reset.h"

#include "../audio/recording_capture.h"

#include "../devices/bmi270.h"
#include "../devices/co5300.h"
#include "../devices/cst820.h"
#include "../devices/es8311.h"
#include "../devices/m5ioe1.h"
#include "../devices/m5pm1.h"
#include "../devices/rx8130ce.h"

#include "../esp32s3/bluetooth/BleHle.h"
#include "../esp32s3/esp32s3_gpio.h"
#include "../esp32s3/esp32s3_i2c.h"
#include "../esp32s3/esp32s3_i2s.h"
#include "../esp32s3/esp32s3_iomux.h"
#include "../esp32s3/esp32s3_spi.h"

#include "../stopwatch_shared.h"

#include <sys/file.h>
#include <sys/mman.h>

enum
{
	DisplayOffsetX = 6,
	I2cSdaPin = 47,
	I2cSclPin = 48,
	MotorPin = 8,
	InputPollIntervalNs = 1000000,
};

typedef struct
{
	DeviceState* panel;
	I2CSlave* touch;
	I2CSlave* imu;
	I2CSlave* pmic;
	I2CSlave* ioe;
	
	bitbang_i2c_interface softwareI2c;
	qemu_irq i2cSdaInput;
	
	qemu_irq buttonA, buttonB, powerButton;
	QEMUTimer* timer;
	
	qemu_irq rtcImuInput;
	bool rtcInterrupt, imuInterrupt;
	
	int sharedFd;
	M5StopWatchShared* shared;
	uint32_t buttons;
	int32_t touchX, touchY;
	bool touchDown;
} StopWatch;

static void i2cDrive(void* opaque, int line, int level)
{
	StopWatch* board = opaque;
	const int sda = bitbang_i2c_set(&board->softwareI2c, line, level != 0);
	qemu_set_irq(board->i2cSdaInput, sda ? -1 : 0);
}

static void rtcImuInterrupt(void* opaque, int source, int level)
{
	StopWatch* board = opaque;
	if (source == 0) board->rtcInterrupt = level == 0;
	else board->imuInterrupt = level != 0;
	
	/* Q7 inverts BMI INT1; its drain shares the RTC open-drain /IRQ net. */
	qemu_set_irq(board->rtcImuInput, !board->rtcInterrupt && !board->imuInterrupt);
}

static void boardTick(void* opaque)
{
	StopWatch* board = opaque;
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	if (board->shared && flock(board->sharedFd, LOCK_EX | LOCK_NB) == 0)
	{
		board->touchDown = board->shared->touchDown != 0;
		board->touchX = board->shared->touchX;
		board->touchY = board->shared->touchY;
		board->buttons = board->shared->buttons;
		bmi270SetMotion(board->imu, board->shared->accelerationG, board->shared->angularVelocityDps);
		
		board->shared->vibration = m5ioe1OutputDuty(board->ioe, MotorPin);
		board->shared->powerLed = m5pm1LedEnabled(board->pmic);
		board->shared->guestTimeNs = nowNs;
		board->shared->completedWindows = co5300CompletedWindows(board->panel);
		board->shared->activeRefreshes = co5300ActiveRefreshes(board->panel);
		
		co5300SetBrightnessSimulation(board->panel, board->shared->simulateBrightness != 0);
		const uint64_t updates = co5300Updates(board->panel);
		if (board->shared->updates != updates)
		{
			co5300ReadPixels(board->panel, board->shared->pixels);
			board->shared->updates = updates;
		}
		flock(board->sharedFd, LOCK_UN);
	}
	
	qemu_set_irq(board->buttonA, !(board->buttons & M5StopWatchButtonA));
	qemu_set_irq(board->buttonB, !(board->buttons & M5StopWatchButtonB));
	qemu_set_irq(board->powerButton, (board->buttons & M5StopWatchButtonPower) != 0);
	cst820SetTouch(board->touch, CLAMP(board->touchX, 0, M5StopWatchWidth - 1), CLAMP(board->touchY, 0, M5StopWatchHeight - 1), board->touchDown);
	timer_mod(board->timer, nowNs + InputPollIntervalNs);
}

static void boardReset(void* opaque)
{
	StopWatch* board = opaque;
	if (board->softwareI2c.current_addr >= 0) i2c_end_transfer(board->softwareI2c.bus);
	bitbang_i2c_init(&board->softwareI2c, board->softwareI2c.bus);
	board->softwareI2c.current_addr = -1;
	board->softwareI2c.state = STOPPED;
	qemu_set_irq(board->i2cSdaInput, -1);
	timer_mod(board->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + InputPollIntervalNs);
}

static void connectFrontend(StopWatch* board)
{
	board->sharedFd = -1;
	const char* path = getenv("M5STOPWATCH_SHARED");
	if (!path) return;
	
	board->sharedFd = open(path, O_RDWR);
	struct stat fileStatus;
	if (board->sharedFd < 0 || fstat(board->sharedFd, &fileStatus) || fileStatus.st_size != sizeof(M5StopWatchShared))
	{
		error_report("StopWatch: invalid shared state file (expected %zu bytes, version %u)", sizeof(M5StopWatchShared), M5StopWatchVersion);
		exit(1);
	}
	
	board->shared = mmap(NULL, sizeof(*board->shared), PROT_READ | PROT_WRITE, MAP_SHARED, board->sharedFd, 0);
	if (board->shared == MAP_FAILED || board->shared->magic != M5StopWatchMagic ||
	    board->shared->version != M5StopWatchVersion || board->shared->width != M5StopWatchWidth || board->shared->height != M5StopWatchHeight)
	{
		error_report("StopWatch: incompatible shared state header (expected version %u)", M5StopWatchVersion);
		exit(1);
	}
}

void m5_stopwatch_init(Object* owner, MemoryRegion* sysmem, ESPGdmaState* gdma, DeviceState* rtcController, qemu_irq spiIrq, qemu_irq gpioIrq, qemu_irq i2c0Irq, qemu_irq i2c1Irq)
{
	bleHleInitMemory(owner, sysmem);
	StopWatch* board = g_new0(StopWatch, 1);
	connectFrontend(board);
	
	DeviceState* gpio = qdev_new(TYPE_ESP32S3_GPIO_CONTROLLER);
	object_property_add_child(owner, "gpio-io", OBJECT(gpio));
	qdev_prop_set_uint32(gpio, "strap-mode", 4);
	qdev_prop_set_uint64(gpio, "external-pullups", (1ULL << I2cSdaPin) | (1ULL << I2cSclPin));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
	memory_region_add_subregion_overlap(sysmem, 0x60004000, sysbus_mmio_get_region(SYS_BUS_DEVICE(gpio), 0), 1);
	sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0, gpioIrq);
	board->buttonA = qdev_get_gpio_in_named(gpio, "in", 2);
	board->buttonB = qdev_get_gpio_in_named(gpio, "in", 1);
	for (unsigned pin = 0; pin < ESP32S3_RTC_GPIO_COUNT; pin++)
	{
		qdev_connect_gpio_out_named(gpio, "pad", pin, qdev_get_gpio_in_named(rtcController, ESP32S3_RTC_GPIO_IN, pin));
	}
	
	DeviceState* iomux = qdev_new(TYPE_ESP32S3_IOMUX);
	object_property_add_child(owner, "iomux", OBJECT(iomux));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(iomux), &error_fatal);
	memory_region_add_subregion_overlap(sysmem, 0x60009000, sysbus_mmio_get_region(SYS_BUS_DEVICE(iomux), 0), 1);
	for (unsigned pin = 0; pin < Esp32s3GpioPinCount; pin++)
	{
		qdev_connect_gpio_out_named(iomux, "pull", pin, qdev_get_gpio_in_named(gpio, "pull", pin));
	}
	
	DeviceState* spi = qdev_new(TYPE_ESP32S3_SPI_CONTROLLER);
	object_property_add_child(owner, "spi2", OBJECT(spi));
	object_property_set_link(OBJECT(spi), "gdma", OBJECT(gdma), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);
	memory_region_add_subregion_overlap(sysmem, 0x60024000, sysbus_mmio_get_region(SYS_BUS_DEVICE(spi), 0), 1);
	sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0, spiIrq);
	for (unsigned cs = 0; cs < Esp32s3SpiCsCount; cs++)
	{
		qdev_connect_gpio_out_named(spi, "cs", cs, qdev_get_gpio_in_named(gpio, "matrix", Esp32s3SpiCs0Signal + cs));
	}
	
	board->panel = qdev_new(TYPE_CO5300);
	qdev_prop_set_uint16(board->panel, "width", M5StopWatchWidth);
	qdev_prop_set_uint16(board->panel, "height", M5StopWatchHeight);
	qdev_prop_set_uint16(board->panel, "x-offset", DisplayOffsetX);
	ssi_realize_and_unref(board->panel, (SSIBus*) qdev_get_child_bus(spi, "spi"), &error_fatal);
	qdev_connect_gpio_out_named(gpio, "out", 39, qdev_get_gpio_in_named(board->panel, SSI_GPIO_CS, 0));
	qdev_connect_gpio_out_named(board->panel, "te", 0, qdev_get_gpio_in_named(gpio, "in", 38));
	
	DeviceState* i2c = qdev_new(TYPE_ESP32S3_I2C);
	object_property_add_child(owner, "i2c0", OBJECT(i2c));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
	memory_region_add_subregion(sysmem, 0x60013000, sysbus_mmio_get_region(SYS_BUS_DEVICE(i2c), 0));
	sysbus_connect_irq(SYS_BUS_DEVICE(i2c), 0, i2c0Irq);
	I2CBus* bus = I2C_BUS(qdev_get_child_bus(i2c, "i2c"));
	bitbang_i2c_init(&board->softwareI2c, bus);
	board->softwareI2c.current_addr = -1;
	board->i2cSdaInput = qdev_get_gpio_in_named(gpio, "in", I2cSdaPin);
	qdev_connect_gpio_out_named(gpio, "drive", I2cSdaPin, qemu_allocate_irq(i2cDrive, board, BITBANG_I2C_SDA));
	qdev_connect_gpio_out_named(gpio, "drive", I2cSclPin, qemu_allocate_irq(i2cDrive, board, BITBANG_I2C_SCL));
	
	/* Either master can drive the board bus. GPIO matrix routing and
	 * simultaneous multi-master access are outside this transaction model. */
	DeviceState* i2c1 = qdev_new(TYPE_ESP32S3_I2C);
	object_property_add_child(owner, "i2c1", OBJECT(i2c1));
	object_property_set_link(OBJECT(i2c1), "bus", OBJECT(bus), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c1), &error_fatal);
	memory_region_add_subregion(sysmem, 0x60027000, sysbus_mmio_get_region(SYS_BUS_DEVICE(i2c1), 0));
	sysbus_connect_irq(SYS_BUS_DEVICE(i2c1), 0, i2c1Irq);
	
	board->pmic = i2c_slave_create_simple(bus, TYPE_M5PM1, 0x6E);
	board->ioe = i2c_slave_create_simple(bus, TYPE_M5IOE1, 0x4F);
	board->touch = i2c_slave_create_simple(bus, TYPE_CST820, 0x15);
	board->imu = i2c_slave_create_simple(bus, TYPE_BMI270, 0x68);
	I2CSlave* rtc = i2c_slave_create_simple(bus, TYPE_RX8130CE, 0x32);
	I2CSlave* codec = i2c_slave_create_simple(bus, TYPE_ES8311, 0x18);
	m5RecordingCaptureInit();
	
	board->powerButton = qdev_get_gpio_in_named(DEVICE(board->pmic), "button", 0);
	board->rtcImuInput = qdev_get_gpio_in_named(DEVICE(board->pmic), "gpio-in", 0);
	qdev_connect_gpio_out_named(DEVICE(rtc), "irq", 0, qemu_allocate_irq(rtcImuInterrupt, board, 0));
	qdev_connect_gpio_out_named(DEVICE(board->imu), "irq", 0, qemu_allocate_irq(rtcImuInterrupt, board, 1));
	qdev_connect_gpio_out_named(DEVICE(board->pmic), "gpio", 1, qdev_get_gpio_in_named(gpio, "in", 12));
	qemu_set_irq(board->rtcImuInput, 1);
	
	qdev_connect_gpio_out_named(DEVICE(board->ioe), "gpio", 2, qdev_get_gpio_in_named(DEVICE(codec), "power", 0));
	qdev_connect_gpio_out_named(DEVICE(board->ioe), "gpio", 9, qdev_get_gpio_in_named(DEVICE(codec), "amplifier", 0));
	
	qdev_connect_gpio_out_named(DEVICE(board->ioe), "drive", 3, qdev_get_gpio_in_named(DEVICE(board->touch), "reset", 0));
	qdev_connect_gpio_out_named(DEVICE(board->ioe), "drive", 4, qdev_get_gpio_in_named(board->panel, "reset", 0));
	qdev_connect_gpio_out_named(DEVICE(board->ioe), "gpio", 7, qdev_get_gpio_in_named(board->panel, "power", 0));
	qdev_connect_gpio_out_named(DEVICE(board->touch), "irq", 0, qdev_get_gpio_in_named(gpio, "in", 13));
	
	/* Touch stays on L2 while IO8 switches the OLED's L3B supply. */
	qemu_set_irq(qdev_get_gpio_in_named(DEVICE(board->touch), "power", 0), 1);
	/* IOE pins start as inputs: an undriven reset is released, not driven low. */
	qemu_set_irq(qdev_get_gpio_in_named(DEVICE(board->touch), "reset", 0), 1);
	qemu_set_irq(qdev_get_gpio_in_named(board->panel, "reset", 0), 1);
	
	DeviceState* i2s = qdev_new(TYPE_ESP32S3_I2S);
	object_property_add_child(owner, "i2s0", OBJECT(i2s));
	object_property_set_link(OBJECT(i2s), "gdma", OBJECT(gdma), &error_fatal);
	object_property_set_link(OBJECT(i2s), "codec", OBJECT(codec), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(i2s), &error_fatal);
	memory_region_add_subregion(sysmem, 0x6000F000, sysbus_mmio_get_region(SYS_BUS_DEVICE(i2s), 0));
	
	board->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, boardTick, board);
	qemu_register_reset(boardReset, board);
	boardReset(board);
}
