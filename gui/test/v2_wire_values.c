/* The CAN Triple 2.0 firmware line's own values for what the Device Manager
 * mirrors of it, read from that line's headers (firmware/v2/include) and
 * handed to test_firmware_v2 as plain numbers.
 *
 * A separate C file, built as its own library with only the 2.0 include
 * directory, because the 2.0 headers and the 1.x ones the Manager compiles
 * define the same names (CMD_*, FW_*, FwUpdateStatus): the two cannot meet in
 * one translation unit, and a numbers-only boundary between them is the whole
 * point of the test. */
#include <stddef.h>

#include "flash_store.h"    /* CapacityReport and its initializer: the unit's own report */
#include "fw_image.h"
#include "protocol.h"
#include "retained_store.h"

const unsigned v2_cmd_fw_confirm = CMD_FW_CONFIRM;
const unsigned v2_cmd_fw_update_status = CMD_FW_UPDATE_STATUS;
const unsigned v2_err_invalid_cmd = ERR_INVALID_CMD;

const unsigned v2_state[3] = {FW_STATE_IDLE, FW_STATE_PENDING, FW_STATE_TRIAL};
const unsigned v2_result[14] = {
    FW_RESULT_NONE,          FW_RESULT_OK,           FW_RESULT_BAD_MAGIC,
    FW_RESULT_WRONG_PRODUCT, FW_RESULT_BAD_SIZE,     FW_RESULT_BAD_CRC,
    FW_RESULT_BL_TOO_OLD,    FW_RESULT_ERASE_FAILED, FW_RESULT_PROGRAM_FAILED,
    FW_RESULT_VERIFY_FAILED, FW_RESULT_GAVE_UP,      FW_RESULT_ROLLED_BACK,
    FW_RESULT_RECOVERED,     FW_RESULT_NO_IMAGE,
};
const unsigned v2_slot[4] = {FW_SLOT_A, FW_SLOT_B, FW_SLOT_FACTORY, FW_SLOT_NONE};
const unsigned v2_slot_count = FW_SLOT_COUNT;

const unsigned v2_status_size = sizeof(FwUpdateStatus);
const unsigned v2_status2_size = sizeof(FwUpdateStatus2);
const unsigned v2_status2_offsets[6] = {
    offsetof(FwUpdateStatus2, running_slot),  offsetof(FwUpdateStatus2, previous_slot),
    offsetof(FwUpdateStatus2, trial_boots),   offsetof(FwUpdateStatus2, slots_valid),
    offsetof(FwUpdateStatus2, failed_version), offsetof(FwUpdateStatus2, slot_version),
};

const unsigned v2_app_max_size = FW_APP_MAX_SIZE;
const unsigned v2_bootloader_version = FW_BOOTLOADER_VERSION;
const unsigned v2_oldest_image_bootloader = FW_OLDEST_IMAGE_BOOTLOADER;
const unsigned v2_product = FW_PRODUCT_THIS_BOARD;

unsigned v2_version_pack(unsigned major, unsigned minor, unsigned patch)
{
    return fw_version_pack((uint16_t)major, (uint16_t)minor, (uint16_t)patch);
}

/* The capacity report exactly as a 2.0 unit sends it and a 2.0 .ctf carries
 * it: the firmware's own initializer, instantiated here. */
static const CapacityReport k_report = CAPACITY_REPORT_INIT;
const unsigned char *const v2_capacity_report = (const unsigned char *)&k_report;
const unsigned v2_capacity_report_size = sizeof(CapacityReport);
const unsigned v2_capacity_format = CAPACITY_REPORT_FORMAT;
const unsigned v2_num_tables = FLASH_NUM_TABLES;
const unsigned v2_capacity_ext_size = sizeof(CapacityExtension);
const unsigned v2_capacity_ext_offsets[9] = {
    offsetof(CapacityExtension, size),
    offsetof(CapacityExtension, retained_values),
    offsetof(CapacityExtension, retained_interval_ms),
    offsetof(CapacityExtension, retained_flags),
    offsetof(CapacityExtension, script_budget),
    offsetof(CapacityExtension, script_op_count),
    offsetof(CapacityExtension, script_op_costs),
    offsetof(CapacityExtension, crc8_features),
    offsetof(CapacityExtension, can_features),
};
/* The CAN buses on the 2.0 line: fast FD data phases (80 MHz clock + TDC). */
const unsigned v2_can_fast_data = CAPACITY_CAN_FAST_DATA;
/* The label store: its commands, kinds and slot width, where the capacity report
 * says so, and the three records it took the names out of, field by field. */
const unsigned v2_label_cmds[2] = {CMD_WRITE_LABELS, CMD_READ_LABELS};
const unsigned v2_label_kinds[3] = {LABEL_KIND_MESSAGE, LABEL_KIND_SIGNAL, LABEL_KIND_RELAY};
const unsigned v2_label_bytes = LABEL_BYTES;
const unsigned v2_capacity_label_at = offsetof(CapacityExtension, label_bytes);
const unsigned v2_record_sizes[3] = {sizeof(CanMessageConfig), sizeof(CanSignalConfig),
                                     sizeof(RelayConfig)};
const unsigned v2_message_offsets[9] = {
    offsetof(CanMessageConfig, can_id),         offsetof(CanMessageConfig, flags),
    offsetof(CanMessageConfig, src_bus),        offsetof(CanMessageConfig, route_bus_mask),
    offsetof(CanMessageConfig, dlc),            offsetof(CanMessageConfig, period_ms),
    offsetof(CanMessageConfig, tx_trigger_cond), offsetof(CanMessageConfig, tx_trigger_flags),
    offsetof(CanMessageConfig, password_slot),
};
const unsigned v2_signal_offsets[10] = {
    offsetof(CanSignalConfig, factor),        offsetof(CanSignalConfig, offset),
    offsetof(CanSignalConfig, min_val),       offsetof(CanSignalConfig, max_val),
    offsetof(CanSignalConfig, default_value), offsetof(CanSignalConfig, mux_id),
    offsetof(CanSignalConfig, mux_mask),      offsetof(CanSignalConfig, msg_and_flags),
    offsetof(CanSignalConfig, tx_source),     offsetof(CanSignalConfig, bits),
};
const unsigned v2_relay_offsets[5] = {
    offsetof(RelayConfig, address), offsetof(RelayConfig, bitmask), offsetof(RelayConfig, flags),
    offsetof(RelayConfig, src_bus), offsetof(RelayConfig, forward_bus_mask),
};
/* The unit's own settings: the commands, the format, the brightness range and
 * the record, field by field. */
const unsigned v2_settings_cmds[2] = {CMD_READ_DEVICE_SETTINGS, CMD_WRITE_DEVICE_SETTINGS};
const unsigned v2_settings_format = DEVICE_SETTINGS_FORMAT;
const unsigned v2_led_brightness_range[2] = {LED_BRIGHTNESS_MIN, LED_BRIGHTNESS_MAX};
const unsigned v2_settings_size = sizeof(DeviceSettings);
const unsigned v2_settings_offsets[3] = {
    offsetof(DeviceSettings, format), offsetof(DeviceSettings, led_brightness),
    offsetof(DeviceSettings, reserved),
};
/* Transmit CRC8 on the 2.0 line: the element spellings, the features the
 * report states, and how many rules the unit holds. */
const unsigned v2_crc8_features = CAPACITY_CRC8_WHOLE_ID | CAPACITY_CRC8_DATA_RUNS;
const unsigned v2_crc8_whole_id = CAPACITY_CRC8_WHOLE_ID;
const unsigned v2_crc8_data_runs = CAPACITY_CRC8_DATA_RUNS;
const unsigned v2_crc8_elem[4] = {CRC8_ELEM_ID, CRC8_ELEM_DATA, CRC8_ELEM_RAW, CRC8_ELEM_ID_ALL};
const unsigned v2_crc8_run[3] = {CRC8_ELEM_DATA_RUN, CRC8_ELEM_RUN_MASK, CRC8_RUN_LAST_MASK};
const unsigned v2_crc8_idall[2] = {CRC8_IDALL_BYTES_MASK, CRC8_IDALL_LSB_FIRST};
const unsigned v2_crc8_first_of_0x7f = CRC8_ELEM_RUN_FIRST(0x7Fu);
const unsigned v2_crc8_max_elements = CRC8_MAX_ELEMENTS;
const unsigned v2_max_crc8 = MAX_CRC8_MESSAGES;
const unsigned v2_crc8_record_size = sizeof(Crc8Config);
/* The script cost model, from script_vm.h: what the unit charges. */
const unsigned v2_script_budget = SCRIPT_TICK_BUDGET;
const unsigned v2_script_op_count = SCRIPT_OP_COST_COUNT;
static const unsigned char k_script_costs[SCRIPT_OP_COST_COUNT] = SCRIPT_OP_COSTS_INIT;
const unsigned char *const v2_script_costs = k_script_costs;
const unsigned v2_retained_no_wear = CAPACITY_RETAINED_NO_WEAR;
const unsigned v2_retained_max = RETAINED_MAX_VALUES;
const unsigned v2_retained_interval_ms = RETAINED_INTERVAL_MS;
const unsigned v2_max_counters = MAX_COUNTERS;
const unsigned v2_max_integrators = MAX_INTEGRATORS;
