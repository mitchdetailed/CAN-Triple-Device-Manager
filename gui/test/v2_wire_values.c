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
#include <string.h>

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
/* The plan block (phase 4): where each of its fields is, and its flag and
 * array sizes. */
const unsigned v2_capacity_plan_offsets[13] = {
    offsetof(CapacityExtension, plan_flags),         offsetof(CapacityExtension, plan_header_bytes),
    offsetof(CapacityExtension, plan_flash_bytes),   offsetof(CapacityExtension, arena_bytes),
    offsetof(CapacityExtension, label_region_bytes), offsetof(CapacityExtension, label_align),
    offsetof(CapacityExtension, ref_bytes),          offsetof(CapacityExtension, ceiling),
    offsetof(CapacityExtension, slice_count),        offsetof(CapacityExtension, measure_count),
    offsetof(CapacityExtension, stage_count),        offsetof(CapacityExtension, stage_table),
    offsetof(CapacityExtension, slice),
};
const unsigned v2_capacity_plan_sizes[5] = {CAPACITY_PLAN_STAGE, CAPACITY_MAX_TABLES,
                                            CAPACITY_MAX_STAGES, CAPACITY_MAX_SLICES,
                                            sizeof(CapacitySlice)};
/* The CAN buses on the 2.0 line: fast FD data phases (80 MHz clock + TDC). */
const unsigned v2_can_fast_data = CAPACITY_CAN_FAST_DATA;
/* The bit rate switch, per CAN FD transmit message. */
const unsigned v2_can_brs_per_message = CAPACITY_CAN_BRS_PER_MESSAGE;
const unsigned v2_msgtx_no_brs = MSGTX_NO_BRS;
/* UDS requests (store v23): the commands, the record, the flags, the limits. */
const unsigned v2_uds[9] = {CMD_WRITE_UDS_CFG, CMD_READ_UDS_CFG, sizeof(UdsConfig),
                            MSGTX_UDS_REQUEST, MSGRX_UDS_RESPONSE, CAPACITY_CAN_UDS_REQUESTS,
                            UDS_REQUEST_MAX, MAX_UDS_REQUESTS, FLASH_NUM_TABLES - 1u /* ENGINE_TABLE_UDS */};
/* The label store: its commands, kinds and slot width, where the capacity report
 * says so, and the three records it took the names out of, field by field. */
const unsigned v2_label_cmds[2] = {CMD_WRITE_LABELS, CMD_READ_LABELS};
const unsigned v2_label_kinds[3] = {LABEL_KIND_MESSAGE, LABEL_KIND_SIGNAL, LABEL_KIND_RELAY};
/* Phase 4, step 3b: the kinds past the names, their flags (and the ones this
 * build reports), a value name's slot and GET_USAGE's label trailer. */
const unsigned v2_label_kinds_3b[5] = {LABEL_KIND_UNIT, LABEL_KIND_VALUE_NAME, LABEL_KIND_COUNT,
                                       LABEL_KINDS_NAMES, VALUE_NAME_BYTES};
const unsigned v2_label_flags[3] = {LABEL_FLAG_UNITS, LABEL_FLAG_VALUE_NAMES, CAPACITY_LABEL_FLAGS};
/* Config Notes: the kind, its flag and its most slots. */
const unsigned v2_label_notes[3] = {LABEL_KIND_NOTE, LABEL_FLAG_NOTES, LABEL_NOTE_SLOTS_MAX};
const unsigned v2_value_name_layout[4] = {sizeof(ValueNameLabel), offsetof(ValueNameLabel, signal),
                                          offsetof(ValueNameLabel, value),
                                          offsetof(ValueNameLabel, name)};
const unsigned v2_usage_labels[3] = {sizeof(UsageLabels), offsetof(UsageLabels, kinds),
                                     offsetof(UsageLabels, slots)};
/* Phase 4, step 4c: the transmit queues in the engine's RAM: where the report
 * says so, its numbers, and GET_USAGE's queue trailer. */
const unsigned v2_capacity_queue_at[2] = {offsetof(CapacityExtension, tx_queue_floor),
                                          offsetof(CapacityExtension, tx_queue_entry_overhead)};
const unsigned v2_tx_queue[4] = {CAPACITY_PLAN_TX_QUEUES, TX_QUEUE_FLOOR_BYTES,
                                 TX_QUEUE_ENTRY_OVERHEAD, TX_QUEUE_MAX_BYTES};
const unsigned v2_usage_queues[3] = {sizeof(UsageQueues), offsetof(UsageQueues, bytes),
                                     offsetof(UsageQueues, need)};
const unsigned v2_capacity_label_flags_at = offsetof(CapacityExtension, label_flags);
const unsigned v2_label_bytes = LABEL_BYTES;
const unsigned v2_capacity_label_at = offsetof(CapacityExtension, label_bytes);
const unsigned v2_record_sizes[3] = {sizeof(CanMessageConfig), sizeof(CanSignalConfig),
                                     sizeof(RelayConfig)};
/* Compound identifiers of 32 bits (store v24): the report's bits, and where
 * the record's upper halves sit. */
const unsigned v2_selector32[5] = {CAPACITY_CAN_SELECTOR_32, CAPACITY_CAN_SELECTOR_32_RX,
                                   sizeof(CanSignalConfig), offsetof(CanSignalConfig, mux_id_hi),
                                   offsetof(CanSignalConfig, mux_mask_hi)};
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
const unsigned v2_crc8_features =
    CAPACITY_CRC8_WHOLE_ID | CAPACITY_CRC8_DATA_RUNS | CAPACITY_CRC8_LONG_RULES;
const unsigned v2_crc8_long_rules = CAPACITY_CRC8_LONG_RULES;
const unsigned v2_crc8_whole_id = CAPACITY_CRC8_WHOLE_ID;
const unsigned v2_crc8_data_runs = CAPACITY_CRC8_DATA_RUNS;
const unsigned v2_crc8_elem[4] = {CRC8_ELEM_ID, CRC8_ELEM_DATA, CRC8_ELEM_RAW, CRC8_ELEM_ID_ALL};
const unsigned v2_crc8_run[3] = {CRC8_ELEM_DATA_RUN, CRC8_ELEM_RUN_MASK, CRC8_RUN_LAST_MASK};
const unsigned v2_crc8_idall[2] = {CRC8_IDALL_BYTES_MASK, CRC8_IDALL_LSB_FIRST};
const unsigned v2_crc8_first_of_0x7f = CRC8_ELEM_RUN_FIRST(0x7Fu);
const unsigned v2_crc8_record_elements = CRC8_RECORD_ELEMENTS;
const unsigned v2_crc8_max_rule_elements = CRC8_MAX_RULE_ELEMENTS;
const unsigned v2_crc8_continuation = CRC8FLAG_CONTINUATION;
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
/* The stored project: its commands, how large one may be, the most one read
 * answers, and where the capacity report says the unit keeps one. */
const unsigned v2_project_cmds[2] = {CMD_WRITE_PROJECT, CMD_READ_PROJECT};
const unsigned v2_project_bytes = PROJECT_BYTES;
const unsigned v2_project_read_max = PROJECT_READ_MAX;
const unsigned v2_capacity_project_at = offsetof(CapacityExtension, project_bytes);
/* What kind of build: the flag, and where the report carries it. This file is
 * built without CT_PRODUCTION, so its report is a development build's. */
const unsigned v2_capacity_build_production = CAPACITY_BUILD_PRODUCTION;
const unsigned v2_capacity_build_at = offsetof(CapacityExtension, build_flags);
/* The plan (phase 4): the store version that brought it, its commands, codes and
 * flags, the records the declaration's ask and the usage report carry, field by
 * field, the arena's measures, and the twelve-bit message index as the unit
 * packs and reads it. */
const unsigned v2_store_version = FLASH_STORE_VERSION;
const unsigned v2_plan_cmds[5] = {CMD_STAGE_PLAN, STAGE_PLAN_FORMAT, STAGE_PLAN_FORMAT_ASK,
                                  CMD_GET_USAGE, USAGE_REPORT_FORMAT};
const unsigned v2_plan_codes[2] = {ERR_NO_ROOM, CONFIG_STATUS_NO_ROOM};
const unsigned v2_plan_short[3] = {PLAN_SHORT_FLASH, PLAN_SHORT_ARENA, PLAN_SHORT_NAMES};
const unsigned v2_usage_flags[3] = {USAGE_REFS_OVERFLOW, USAGE_SEND_OPEN, USAGE_NAMES_KEPT};
const unsigned v2_plan_cost_size = sizeof(PlanCost);
const unsigned v2_plan_cost_offsets[7] = {
    offsetof(PlanCost, format),      offsetof(PlanCost, verdict),     offsetof(PlanCost, short_of),
    offsetof(PlanCost, reserved),    offsetof(PlanCost, flash_bytes), offsetof(PlanCost, arena_bytes),
    offsetof(PlanCost, label_bytes),
};
const unsigned v2_usage_header_size = sizeof(UsageReportHeader);
const unsigned v2_usage_header_offsets[11] = {
    offsetof(UsageReportHeader, format),        offsetof(UsageReportHeader, table_count),
    offsetof(UsageReportHeader, config_status), offsetof(UsageReportHeader, flags),
    offsetof(UsageReportHeader, flash_bytes),   offsetof(UsageReportHeader, arena_bytes),
    offsetof(UsageReportHeader, ref_bytes),     offsetof(UsageReportHeader, label_bytes),
    offsetof(UsageReportHeader, retained_used), offsetof(UsageReportHeader, retained_room),
    offsetof(UsageReportHeader, project_bytes),
};
const unsigned v2_usage_entry_size = sizeof(UsageEntry);
const unsigned v2_usage_entry_offsets[2] = {offsetof(UsageEntry, capacity), offsetof(UsageEntry, used)};
const unsigned v2_measures[12] = {
    ARENA_MEASURE_CHANNEL,      ARENA_MEASURE_CHANNEL_END, ARENA_MEASURE_CHANNEL_BIT,
    ARENA_MEASURE_MESSAGE,      ARENA_MEASURE_MESSAGE_BIT, ARENA_MEASURE_MESSAGE_WORD,
    ARENA_MEASURE_CONDITION,    ARENA_MEASURE_COUNTER,     ARENA_MEASURE_TIMER,
    ARENA_MEASURE_INTEGRATOR,   ARENA_MEASURE_STAGE_ROW_BIT, ARENA_MEASURE_COUNT,
};
const unsigned v2_sig_msg[4] = {SIG_MSG_HI_SHIFT, SIG_MSG_HI_MASK, SIG_MSG_IDX12_NONE, SIG_MSG_MAX};

unsigned v2_sig_set_header(unsigned msg_and_flags, unsigned msg_idx, unsigned byte_order,
                           unsigned is_active)
{
    CanSignalConfig s;
    memset(&s, 0, sizeof(s));
    s.msg_and_flags = (uint16_t)msg_and_flags;
    sig_set_header(&s, (uint16_t)msg_idx, (uint8_t)byte_order, (uint8_t)is_active);
    return s.msg_and_flags;
}

/* Any channel kept across power cycles (phase 4, step 3a): the record's flag
 * and the report's, and what sig_set_bits does to a word that has the flag. */
const unsigned v2_retain_bits[2] = {SIG_BITS_RETAIN, CAPACITY_RETAINED_CHANNELS};

unsigned v2_sig_set_bits_on(unsigned bits)
{
    CanSignalConfig s;
    memset(&s, 0, sizeof(s));
    s.bits = (uint32_t)bits;
    sig_set_bits(&s, 5u, 16u, SIGNAL_TYPE_UINT16, 2, 3u);
    return s.bits;
}

unsigned v2_sig_msg_idx(unsigned msg_and_flags)
{
    CanSignalConfig s;
    memset(&s, 0, sizeof(s));
    s.msg_and_flags = (uint16_t)msg_and_flags;
    return sig_msg_idx(&s);
}
