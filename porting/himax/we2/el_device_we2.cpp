/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 (Seeed Technology Inc.)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "el_device_we2.h"

extern "C" {
#include <WE2_ARMCM55.h>
#include <WE2_core.h>
#include <ethosu_driver.h>
#include <hx_drv_pmu.h>
#include <hx_drv_watchdog.h>
#include <spi_eeprom_comm.h>
}

#include <cstdint>
#include <cstring>

#include "core/el_debug.h"
#include "el_camera_we2.h"
#include "el_config_porting.h"
#include "el_serial2_we2.h"
#include "el_serial_we2.h"
#include "el_sspi_we2.h"
#include "el_wire_we2.h"
#include "porting/el_flash.h"

#define U55_BASE             BASE_ADDR_APB_U55_CTRL_ALIAS
#define WATCH_DOG_TIMEOUT_TH 2000

namespace edgelab {

namespace porting {

extern bool _el_flash_init();
extern bool _el_flash_enable_xip();

static inline uint32_t _device_id_from_flash() {
    if (!_el_flash_init()) [[unlikely]]
        return 0ul;

    if (!_el_flash_enable_xip()) [[unlikely]]
        return 0ul;

    uint8_t id_full[16]{};

    std::memcpy(id_full, reinterpret_cast<uint8_t*>(0x3A000000 + 0x003DF000), sizeof id_full);

    // Fowler–Noll–Vo hash function
    uint32_t hash  = 0x811c9dc5;
    uint32_t prime = 0x1000193;
    for (size_t i = 0; i < 16; ++i) {
        uint8_t value = id_full[i];
        hash          = hash ^ value;
        hash *= prime;
    }

    return hash;
}

struct ethosu_driver _ethosu_drv; /* Default Ethos-U device driver */

static void _arm_npu_irq_handler(void) {
    /* Call the default interrupt handler from the NPU driver */
    ethosu_irq_handler(&_ethosu_drv);
}

static void _arm_npu_irq_init(void) {
    const IRQn_Type ethosu_irqnum = (IRQn_Type)U55_IRQn;

    /* Register the EthosU IRQ handler in our vector table.
     * Note, this handler comes from the EthosU driver */
    EPII_NVIC_SetVector(ethosu_irqnum, (uint32_t)_arm_npu_irq_handler);

    /* Enable the IRQ */
    NVIC_EnableIRQ(ethosu_irqnum);
}

static int _arm_npu_init(bool security_enable, bool privilege_enable) {
    int err = 0;

    /* Initialise the IRQ */
    _arm_npu_irq_init();

    /* Initialise Ethos-U55 device */
    void* const ethosu_base_address = (void*)(U55_BASE);

    if (0 != (err = ethosu_init(&_ethosu_drv,         /* Ethos-U driver device pointer */
                                ethosu_base_address,  /* Ethos-U NPU's base address. */
                                NULL,                 /* Pointer to fast mem area - NULL for U55. */
                                0,                    /* Fast mem region size. */
                                security_enable,      /* Security enable. */
                                privilege_enable))) { /* Privilege enable. */
        EL_LOGD("Failed to initalise Ethos-U device");
        return err;
    }

    EL_LOGD("Ethos-U55 device initialised");

    return 0;
}

void WDG_Reset_ISR_CB(uint32_t event) {
    el_printf("Watchdog reset\r\n");
    Device::get_device()->reset();
}

}  // namespace porting

DeviceWE2::DeviceWE2() { init(); }

void DeviceWE2::init() {
    size_t wakeup_event{};
    size_t wakeup_event1{};

    hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT, &wakeup_event);
    hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT1, &wakeup_event1);
    EL_LOGD("wakeup_event=0x%x,WakeupEvt1=0x%x", wakeup_event, wakeup_event1);

    WATCHDOG_CFG_T wdg_cfg;
    wdg_cfg.period = WATCH_DOG_TIMEOUT_TH;
    wdg_cfg.ctrl   = WATCHDOG_CTRL_CPU;
    wdg_cfg.state  = WATCHDOG_STATE_DC;
    wdg_cfg.type   = WATCHDOG_RESET;  //wewweWATCHDOG_INT;
    hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg, porting::WDG_Reset_ISR_CB);
    hx_drv_uart_init(USE_DW_UART_0, HX_UART0_BASE);

    porting::_arm_npu_init(true, true);

    this->_device_name = PORT_DEVICE_NAME;
    this->_device_id   = porting::_device_id_from_flash();
    this->_revision_id = 0x0001;

    static uint8_t sensor_id = 0;

    static CameraWE2 camera{};
    this->_camera = &camera;
    this->_registered_sensors.emplace_front(el_sensor_info_t{
      .id = ++sensor_id, .type = el_sensor_type_t::EL_SENSOR_TYPE_CAM, .state = el_sensor_state_t::EL_SENSOR_STA_REG});

    static SerialWE2 serial{};
    serial.type = EL_TRANSPORT_UART;
    this->_transports.emplace_front(&serial);

    this->_network = nullptr;

#ifndef CONFIG_EL_BOARD_GROVE_VISION_AI_WE2
    static sspiWE2 spi{};
    spi.type = EL_TRANSPORT_SPI;
    this->_transports.emplace_front(&spi);
#endif

#ifdef CONFIG_EL_BOARD_GROVE_VISION_AI_WE2
    static Serial2WE2 serial2{};
    serial2.type = EL_TRANSPORT_UART;
    this->_transports.emplace_front(&serial2);

    static WireWE2 wire{0x62};
    wire.type = EL_TRANSPORT_I2C;
    this->_transports.emplace_front(&wire);
#endif
}

void DeviceWE2::reset() { __NVIC_SystemReset(); }

void DeviceWE2::enter_bootloader() {
    el_printf("Enter bootloader\r\n");
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI2AHB_DO, 1);
    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI2AHB_DI, 1);
    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI2AHB_SCLK, 1);
    hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI2AHB_CS, 1);
}

void DeviceWE2::feed_watchdog() { hx_drv_watchdog_update(WATCHDOG_ID_0, WATCH_DOG_TIMEOUT_TH); }

Device* Device::get_device() {
    static DeviceWE2 device{};
    return &device;
}

}  // namespace edgelab

// current implementation does not support multiple cameras
void __on_algo_preprocess_done() { edgelab::DeviceWE2::get_device()->get_camera()->stop_stream(); }
