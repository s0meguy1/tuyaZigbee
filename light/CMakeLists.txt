################################
# Common settings
cmake_minimum_required(VERSION 3.22)

option(MOES_BENCH_SELFTEST
    "Build the TS0505B BENCH ONLY / DO NOT INSTALL ON LIGHT self-test image"
    OFF)

SET(TARGET light)

IF(DEVICE_VARIANT)
    SET(TARGET ${TARGET}_${DEVICE_VARIANT})
ELSE()
    SET(DEVICE_VARIANT TS0501B)
ENDIF()

IF(NOT MIMIC_HEIMAN)
    SET(MIMIC_HEIMAN 0)
ELSE()
    SET(TARGET ${TARGET}_heiman)
ENDIF()

# The self-test is intentionally a different target and never produces an
# OTA container. It is only for a loose, sacrificial ZT3L on the bench: do
# not make it available to another device variant or under a mimic identity.
IF(MOES_BENCH_SELFTEST)
    IF(NOT DEVICE_VARIANT STREQUAL "TS0505B")
        MESSAGE(FATAL_ERROR "MOES_BENCH_SELFTEST is only supported for DEVICE_VARIANT=TS0505B")
    ENDIF()
    IF(MIMIC_HEIMAN)
        MESSAGE(FATAL_ERROR "MOES_BENCH_SELFTEST cannot be combined with MIMIC_HEIMAN")
    ENDIF()
    SET(TARGET ${TARGET}_BENCH_ONLY_DO_NOT_INSTALL_ON_LIGHT)
    SET(MOES_BENCH_SELFTEST_VALUE 1)
    MESSAGE(WARNING "Building BENCH ONLY self-test. DO NOT INSTALL ON A LIGHT; no OTA artifact will be made.")
ELSE()
    SET(MOES_BENCH_SELFTEST_VALUE 0)
ENDIF()

# The TS0505B depends on five SDK fixes for NV placement, IEEE preservation,
# bounded MSPI waits, and OTA commit/erase safety. Refuse a hand-edited or
# unpatched SDK at configure time, before any object is compiled. Other legacy
# board targets retain their upstream SDK behaviour.
IF(DEVICE_VARIANT STREQUAL "TS0505B")
    EXECUTE_PROCESS(
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/apply_sdk_patches.py"
                --sdk-root "${SDK_PREFIX}" --verify-only
        RESULT_VARIABLE MOES_SDK_VERIFY_RESULT
        OUTPUT_VARIABLE MOES_SDK_VERIFY_OUTPUT
        ERROR_VARIABLE MOES_SDK_VERIFY_ERROR)
    IF(NOT MOES_SDK_VERIFY_RESULT EQUAL 0)
        MESSAGE(FATAL_ERROR
            "TS0505B requires the exact recorded MOES SDK patch set.\n${MOES_SDK_VERIFY_OUTPUT}${MOES_SDK_VERIFY_ERROR}")
    ENDIF()
    MESSAGE(STATUS "${MOES_SDK_VERIFY_OUTPUT}")
    MATH(EXPR MOES_TS0505B_FILE_VERSION
        "(0x11 << 24) + (${MOES_APP_BUILD_EFFECTIVE} << 16) + (0x30 << 8) + 0x03"
        OUTPUT_FORMAT HEXADECIMAL)
    SET(MOES_TS0505B_SW_BUILD_ID "v1.${MOES_APP_BUILD_EFFECTIVE}s3.3")
ENDIF()

ADD_DEFINITIONS(
    -DEND_DEVICE=1
    -DMCU_CORE_${TELINK_PLATFORM}=1
    -DMCU_STARTUP_${TELINK_PLATFORM}=1
    -D__PROJECT_TL_DIMMABLE_LIGHT__=1
    -DBUILD_${DEVICE_VARIANT}=1
    -DROUTER=1
    -DMIMIC_HEIMAN=${MIMIC_HEIMAN}
)

# =======================================================================
# OTA-CRITICAL. Prove any change below on the SWire bench before it
# reaches a live fixture. This single constant decides where the OTA
# staging bank lands; get it wrong and every deployed fixture takes one
# more update and then can never take another, which is not recoverable
# over the air. See the full banner in light/moes_otaScheme.h,
# MOES_EDITING_GUIDE.md S1.1 and S1.2, and OTA_TEST_PLAN.md Phase 0.
# =======================================================================
# Moes TS0505B: relocate NV to Tuya's own base. Two reasons, both fatal
# otherwise (see the patch note in tl_zigbee_sdk/proj/drivers/drv_nv.h):
#   * the SDK default 0xE6000 makes NV overrun the factory blocks at
#     0xF8000 (board config) and 0xFB000 (identity/IEEE)
#   * it also puts the OTA staging bank at 0x77000, while the stock Tuya
#     bootloader reads 0x70000 - the "cannot update again" bug
# 0xD8000 yields NV 0xD8000-0xEE000 and OTA bank exactly 0x70000.
IF(DEVICE_VARIANT MATCHES "^TS0505B")
    ADD_DEFINITIONS(-DMOES_NV_BASE_ADDRESS=0xD8000)
ENDIF()

################################
# Zigbee Library

IF(MOES_BENCH_SELFTEST)
    # Keep the bench ELF intentionally small and independent of the Zigbee
    # application. The platform boot/vector files, direct PWM driver, and
    # software-printf implementation are the only SDK source we need.
    SET(ZIGBEE_SRC
        ${SDK_PREFIX}/platform/boot/link_cfg.S
        ${SDK_PREFIX}/platform/boot/${TELINK_PLATFORM}/cstartup_${TELINK_PLATFORM}.S
        ${SDK_PREFIX}/platform/tc32/div_mod.S
        ${SDK_PREFIX}/platform/chip_${TELINK_PLATFORM}/flash.c
        ${SDK_PREFIX}/platform/chip_${TELINK_PLATFORM}/adc.c
        ${SDK_PREFIX}/platform/chip_${TELINK_PLATFORM}/flash/flash_mid13325e.c
        ${SDK_PREFIX}/proj/common/tlPrintf.c
        ${SDK_PREFIX}/proj/drivers/drv_putchar.c
        ${SDK_PREFIX}/proj/drivers/drv_pwm.c
    )
ELSE()
SET(ZIGBEE_SRC
    ${SDK_PREFIX}/platform/boot/link_cfg.S
    ${SDK_PREFIX}/platform/boot/${TELINK_PLATFORM}/cstartup_${TELINK_PLATFORM}.S
    ${SDK_PREFIX}/platform/services/b85m/irq_handler.c
    ${SDK_PREFIX}/platform/tc32/div_mod.S
    ${SDK_PREFIX}/platform/chip_${TELINK_PLATFORM}/flash.c
    ${SDK_PREFIX}/platform/chip_${TELINK_PLATFORM}/adc.c
    ${SDK_PREFIX}/platform/chip_${TELINK_PLATFORM}/flash/flash_mid13325e.c

    ${SDK_PREFIX}/zigbee/bdb/bdb.c
    ${SDK_PREFIX}/zigbee/aps/aps_group.c
    ${SDK_PREFIX}/zigbee/mac/mac_phy.c
    ${SDK_PREFIX}/zigbee/mac/mac_pib.c
    ${SDK_PREFIX}/zigbee/zdo/zdp.c
    ${SDK_PREFIX}/zigbee/zcl/zcl.c
    ${SDK_PREFIX}/zigbee/zcl/zcl_nv.c
    ${SDK_PREFIX}/zigbee/zcl/zcl_reporting.c
    ${SDK_PREFIX}/zigbee/zcl/smart_energy/zcl_metering.c
    ${SDK_PREFIX}/zigbee/zcl/smart_energy/zcl_metering_attr.c
    ${SDK_PREFIX}/zigbee/zcl/zll_commissioning/zcl_toucklink_security.c
    ${SDK_PREFIX}/zigbee/zcl/zll_commissioning/zcl_zllTouchLinkDiscovery.c
    ${SDK_PREFIX}/zigbee/zcl/zll_commissioning/zcl_zllTouchLinkJoinOrStart.c
    ${SDK_PREFIX}/zigbee/zcl/zll_commissioning/zcl_zll_commissioning.c
    ${SDK_PREFIX}/zigbee/zcl/commissioning/zcl_commissioning.c
    ${SDK_PREFIX}/zigbee/zcl/commissioning/zcl_commissioning_attr.c
    ${SDK_PREFIX}/zigbee/zcl/hvac/zcl_thermostat.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_electrical_measurement.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_electrical_measurement_attr.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_illuminance_measurement.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_illuminance_measurement_attr.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_occupancy_sensing.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_occupancy_sensing_attr.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_temperature_measurement.c
    ${SDK_PREFIX}/zigbee/zcl/measument_sensing/zcl_temperature_measurement_attr.c
    ${SDK_PREFIX}/zigbee/zcl/light_color_control/zcl_light_colorCtrl.c
    ${SDK_PREFIX}/zigbee/zcl/light_color_control/zcl_light_colorCtrl_attr.c
    ${SDK_PREFIX}/zigbee/zcl/closures/zcl_door_lock.c
    ${SDK_PREFIX}/zigbee/zcl/closures/zcl_door_lock_attr.c
    ${SDK_PREFIX}/zigbee/zcl/closures/zcl_window_covering.c
    ${SDK_PREFIX}/zigbee/zcl/closures/zcl_window_covering_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_alarm.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_alarm_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_basic.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_basic_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_binary_input.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_binary_input_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_binary_output.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_binary_output_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_devTemperatureCfg.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_devTemperatureCfg_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_diagnostics.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_diagnostics_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_greenPower.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_greenPower_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_group.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_group_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_identify.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_identify_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_level.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_level_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_multistate_input.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_multistate_input_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_multistate_output.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_multistate_output_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_onoff.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_onoff_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_pollCtrl.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_pollCtrl_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_powerCfg.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_powerCfg_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_scene.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_scene_attr.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_time.c
    ${SDK_PREFIX}/zigbee/zcl/general/zcl_time_attr.c
    ${SDK_PREFIX}/zigbee/zcl/security_safety/zcl_ias_ace.c
    ${SDK_PREFIX}/zigbee/zcl/security_safety/zcl_ias_wd.c
    ${SDK_PREFIX}/zigbee/zcl/security_safety/zcl_ias_wd_attr.c
    ${SDK_PREFIX}/zigbee/zcl/security_safety/zcl_ias_zone.c
    ${SDK_PREFIX}/zigbee/zcl/security_safety/zcl_ias_zone_attr.c
    ${SDK_PREFIX}/zigbee/zcl/zcl_wwah/zcl_wwah.c
    ${SDK_PREFIX}/zigbee/zcl/zcl_wwah/zcl_wwah_attr.c
    ${SDK_PREFIX}/zigbee/zcl/ota_upgrading/zcl_ota.c
    ${SDK_PREFIX}/zigbee/zcl/ota_upgrading/zcl_ota_attr.c
    ${SDK_PREFIX}/zigbee/common/zb_config.c
    ${SDK_PREFIX}/zigbee/af/zb_af.c
    ${SDK_PREFIX}/zigbee/wwah/wwah.c
    ${SDK_PREFIX}/zigbee/wwah/wwahEpCfg.c
    ${SDK_PREFIX}/zigbee/gp/gp.c
    ${SDK_PREFIX}/zigbee/gp/gpEpCfg.c
    ${SDK_PREFIX}/zigbee/gp/gp_proxy.c
    ${SDK_PREFIX}/zigbee/gp/gp_proxyTab.c
    ${SDK_PREFIX}/zigbee/ss/ss_nv.c
    ${SDK_PREFIX}/zigbee/ota/ota.c
    ${SDK_PREFIX}/zigbee/ota/otaEpCfg.c



    ${SDK_PREFIX}/zbhci/zbhci.c
    ${SDK_PREFIX}/zbhci/zbhciCmdProcess.c
    ${SDK_PREFIX}/zbhci/zbhci_zclHandler.c
    ${SDK_PREFIX}/zbhci/uart/hci_uart.c
    ${SDK_PREFIX}/zbhci/usbCDC/zbhci_usb_cdc.c
    ${SDK_PREFIX}/zbhci/usbHID/zbhci_usb_hid.c
    ${SDK_PREFIX}/zbhci/usbPrint/hci_usbPrint.c


    ${SDK_PREFIX}/proj/common/list.c
    ${SDK_PREFIX}/proj/common/mempool.c
    ${SDK_PREFIX}/proj/common/string.c
    ${SDK_PREFIX}/proj/common/tlPrintf.c
    ${SDK_PREFIX}/proj/common/utility.c
    ${SDK_PREFIX}/proj/drivers/drv_adc.c
    ${SDK_PREFIX}/proj/drivers/drv_flash.c
    ${SDK_PREFIX}/proj/drivers/drv_calibration.c
    ${SDK_PREFIX}/proj/drivers/drv_gpio.c
    ${SDK_PREFIX}/proj/drivers/drv_hw.c
    ${SDK_PREFIX}/proj/drivers/drv_i2c.c
    ${SDK_PREFIX}/proj/drivers/drv_keyboard.c
    ${SDK_PREFIX}/proj/drivers/drv_nv.c
    ${SDK_PREFIX}/proj/drivers/drv_pm.c
    ${SDK_PREFIX}/proj/drivers/drv_putchar.c
    ${SDK_PREFIX}/proj/drivers/drv_pwm.c
    ${SDK_PREFIX}/proj/drivers/drv_spi.c
    ${SDK_PREFIX}/proj/drivers/drv_timer.c
    ${SDK_PREFIX}/proj/drivers/drv_uart.c
    ${SDK_PREFIX}/proj/os/ev.c
    ${SDK_PREFIX}/proj/os/ev_buffer.c
    ${SDK_PREFIX}/proj/os/ev_poll.c
    ${SDK_PREFIX}/proj/os/ev_queue.c
    ${SDK_PREFIX}/proj/os/ev_timer.c

    ${SDK_PREFIX}/zigbee/common/zb_version.c
)
ENDIF()

# The normal image uses Telink's stack main. The bench image has a separate
# main that deliberately never starts the stack or its periodic task runner.
IF(NOT MOES_BENCH_SELFTEST)
    LIST(APPEND ZIGBEE_SRC ${SDK_PREFIX}/apps/common/main.c)
ENDIF()


################################
# Telink Zigbee device

INCLUDE_DIRECTORIES(
    ${PROJECT_SOURCE_DIR}

    ${SDK_PREFIX}/zigbee/af
    ${SDK_PREFIX}/zigbee/bdb/includes
    ${SDK_PREFIX}/zigbee/ota
    ${SDK_PREFIX}/zigbee/zcl
    ${SDK_PREFIX}/zigbee/common/includes
    ${SDK_PREFIX}/zigbee/zbapi
    ${SDK_PREFIX}/zigbee/gp
    ${SDK_PREFIX}/proj
    ${SDK_PREFIX}/proj/common
    ${SDK_PREFIX}/platform

    # Zigbee sources reference zcl_options.h
    ${PROJECT_SOURCE_DIR}/common
    ${PROJECT_SOURCE_DIR}/light
)

LINK_DIRECTORIES(
	${SDK_PREFIX}/zigbee/lib/tc32
	${SDK_PREFIX}/platform/lib
)

SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T${SDK_PREFIX}/platform/boot/${TELINK_PLATFORM}/boot_${TELINK_PLATFORM}.link")

IF(MOES_BENCH_SELFTEST)
    # The upstream toolchain forces this Zigbee security symbol into every
    # normal application. Removing that linker-only root keeps the bench ELF
    # from pulling in the stack merely because it is linked in the same tree.
    STRING(REPLACE "-Wl,-uss_apsmeSwitchKeyReq" "" CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS}")
ENDIF()

IF(MOES_BENCH_SELFTEST)
    # Do not even compile the normal application objects into the bench link:
    # several SDK callbacks live in retained/RAM sections and would otherwise
    # retain unresolved BDB/NV/OTA references despite being unreachable from
    # the bench main loop.
    SET(SOURCES1
        moes_bench_main.c
        moes_bench_selftest.c
        moes_bench_irq.c
    )
ELSE()
    file( GLOB SOURCES1 *.c *.cpp *.h *.S ../common/*.c ../common/*.h )
ENDIF()

SET (SOURCES  ${SOURCES1} ${ZIGBEE_SRC})

ADD_EXECUTABLE(${TARGET} ${SOURCES})
TARGET_COMPILE_DEFINITIONS(${TARGET} PRIVATE
    MOES_BENCH_SELFTEST=${MOES_BENCH_SELFTEST_VALUE})
TARGET_LINK_LIBRARIES(${TARGET}
        -Wl,--start-group

	#ZigBee

	zb_router
	drivers_${TELINK_PLATFORM}
#        soft-fp
	timestamp

        -Wl,--end-group
)

IF(DEVICE_VARIANT STREQUAL "TS0505B")
    IF(MOES_BENCH_SELFTEST)
        ADD_BIN_TARGET(${TARGET} ${PROJECT_SOURCE_DIR}/tools
            EXPECTED_MANUFACTURER 0x6464
            EXPECTED_IMAGE_TYPE 0x0395
            EXPECTED_FILE_VERSION ${MOES_TS0505B_FILE_VERSION})
    ELSE()
        ADD_BIN_TARGET(${TARGET} ${PROJECT_SOURCE_DIR}/tools
            EXPECTED_MANUFACTURER 0x6464
            EXPECTED_IMAGE_TYPE 0x0395
            EXPECTED_FILE_VERSION ${MOES_TS0505B_FILE_VERSION}
            EXPECTED_SW_BUILD_ID ${MOES_TS0505B_SW_BUILD_ID})
    ENDIF()
ELSE()
    ADD_BIN_TARGET(${TARGET} ${PROJECT_SOURCE_DIR}/tools)
ENDIF()
IF(NOT MOES_BENCH_SELFTEST)
    IF(DEVICE_VARIANT STREQUAL "TS0505B")
        ADD_OTA_TARGET(${TARGET} ${PROJECT_SOURCE_DIR}/tools
            EXPECTED_MANUFACTURER 0x6464
            EXPECTED_IMAGE_TYPE 0x0395
            EXPECTED_FILE_VERSION ${MOES_TS0505B_FILE_VERSION}
            EXPECTED_SW_BUILD_ID ${MOES_TS0505B_SW_BUILD_ID})
    ELSE()
        ADD_OTA_TARGET(${TARGET} ${PROJECT_SOURCE_DIR}/tools)
    ENDIF()
ENDIF()

# TS0505B: also build the from-stock conversion OTA - served to stock Tuya
# firmware it must present the stock query pair (0x1141 / 0xD3A3) with a
# version no stock build can exceed, so every deployed light accepts it.
IF(DEVICE_VARIANT STREQUAL "TS0505B" AND NOT MOES_BENCH_SELFTEST)
    ADD_TUYA_OTA_TARGET(${TARGET} ${PROJECT_SOURCE_DIR}/tools 0x1141 0xD3A3 0xFFFFFFFF
        EXPECTED_MANUFACTURER 0x6464
        EXPECTED_IMAGE_TYPE 0x0395
        EXPECTED_FILE_VERSION ${MOES_TS0505B_FILE_VERSION}
        EXPECTED_SW_BUILD_ID ${MOES_TS0505B_SW_BUILD_ID})
ENDIF()
