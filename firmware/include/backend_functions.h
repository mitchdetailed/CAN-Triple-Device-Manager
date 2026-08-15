/*
 * backend_functions.h
 */

/* Defines Declarations */
#ifndef INC_BACKEND_FUNCTIONS_H_
#define INC_BACKEND_FUNCTIONS_H_

/* Includes Declarations */
#include "stm32g4xx_hal.h"
#include "user_code.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint32_t timestamp;
/* Variable Declarations */

extern volatile uint16_t can1Reset_counter;
extern volatile uint16_t can2Reset_counter;
extern volatile uint16_t can3Reset_counter;
extern volatile uint8_t can_reset_flags; // Bitmask: set in ISR, cleared by handle_CAN_errors()

typedef enum {
  CAN_1 = 1 << 0,
  CAN_2 = 1 << 1,
  CAN_3 = 1 << 2,
  CAN_4 = 1 << 3,
  CAN_5 = 1 << 4,
  CAN_6 = 1 << 5,
  CAN_7 = 1 << 6,
  CAN_8 = 1 << 7,
  CAN_9 = 1 << 8
} CAN_Bus;

typedef enum { LED_1 } gpio_LED;

typedef struct {
  uint8_t TxErrorCounter;
  uint8_t RxErrorCounter;
  uint8_t BusResetCounter;
} CAN_ErrorCounts;

// Define datatype_t enum
typedef enum {
  DBC_UNSIGNED = 0,
  DBC_SIGNED = 1,
  DBC_FLOAT = 2,
  DBC_DOUBLE = 4
} datatype_t;

// Union for IEEE-754 float representation
typedef union {
  uint8_t bytes[4];
  float f32;
} FloatUnion_t;

// Union for IEEE-754 double representation
typedef union {
  uint8_t bytes[8];
  double f64;
} DoubleUnion_t;

/**
 * @brief Structure to represent a CAN network message.
 * \param Bus : ID of the CAN bus the message is associated with.
 * \param is_extended_id : True if using an extended ID, false if using a
 * standard ID.
 * \param arbitration_id : The identifier for the message, either standard or
 * extended based on is_extended_id.
 * \param dlc : the Data Length in bytes (1-64)
 * \param data : the Data array, sized to the largest configured bus DLC
 * \return uint32_t Value
 */
typedef struct {
  uint16_t Bus;        /**< ID of the CAN bus the message is associated with. */
  bool is_extended_id; /**< True if using an extended ID, false if using a
                          standard ID. */
  uint32_t arbitration_id; /**< The identifier for the message, either standard
                              or extended based on is_extended_id. */
  uint8_t data_length; /**< Data length in bytes, the number of valid bytes in
                  the data field. */
  uint8_t data[64];    /**< Data payload of the CAN message. */
  bool brs; /**< True if Bit Rate Switch (BRS) is enabled, false otherwise. */
  bool esi; /**< CAN FD error-state indicator: true if the transmitter was
                 error-passive when the frame was sent. */
  bool fd;  /**< True if the frame is CAN FD FORMAT, independent of length and
                 of BRS. Before this bit existed the format was inferred from
                 data_length > 8, which silently demoted an FD frame of 8 bytes
                 or fewer to classic on transmit — a relay forwarding such a
                 frame changed its format on the wire. */
} CAN_Message;

typedef struct {
  uint16_t Head;
  uint16_t Tail;
  uint16_t ElementsAvailable; // Number of valid messages
  uint16_t BytesUsed;         // Number of bytes currently written in Memory
  uint16_t MissedMsgs;        // Number of messages rejected due to full buffer
  uint16_t Size;              // Total capacity
  bool OverwriteMode;         // true = overwrite oldest, false = reject
  uint8_t *Memory;
} CAN_MessageBuffer;

typedef struct __attribute__((packed)) {
  uint32_t arbitration_id; // Bits 0-28: ID, Bit 29: ESI, Bit 30: BRS, Bit 31: EXT_ID
  uint8_t data_length;
} CAN_PackedHeader;

// MSB = 1 for TX, 0 for RX. Lower 15 bits = Bus mask.
#define BUFFER_RX_TX_MASK 0x8000

extern CAN_MessageBuffer rx_buffers[15];
extern CAN_MessageBuffer tx_buffers[15];

typedef enum { NORMAL_MODE, LISTEN_ONLY } CAN_Mode;

typedef struct {
  char array[UART_MSG_BUFFER_SIZE];
  uint16_t length;
} StringArray;

extern StringArray array0;
extern StringArray array1;
extern volatile uint8_t uart_array;

/* Function Prototypes */

typedef enum {
    TRIPLE_OK = 0x00,               // Success
    TRIPLE_ERR_ERROR = 0x01,        // General error
    TRIPLE_ERR_BUSY = 0x02,         // Hardware/Resource is busy
    TRIPLE_ERR_TIMEOUT = 0x03,      // Operation timed out
    TRIPLE_ERR_INVALID_ARG = 0x04,  // Invalid parameter (e.g., bad Bus ID)
    TRIPLE_ERR_BUFFER_FULL = 0x05,  // Circular buffer overflow
    TRIPLE_ERR_BUFFER_EMPTY = 0x06, // Circular buffer underflow
    TRIPLE_ERR_HW_INIT = 0x07       // Hardware initialization failed
} TRIPLE_Status;

// CAN Physical Layer Function Prototypes //
TRIPLE_Status setupCANbus(CAN_Bus bus, uint32_t mainBitrate, CAN_Mode mode);
TRIPLE_Status setupCAN_FDbus(CAN_Bus bus, uint32_t mainBitrate, float mainSP,
                       uint32_t dataBitrate, float dataSP, bool bitrateSwitch,
                       CAN_Mode mode, uint8_t txDelayComp);
TRIPLE_Status startCANbus(CAN_Bus bus);
TRIPLE_Status stopCANbus(CAN_Bus bus);
TRIPLE_Status resetCAN(CAN_Bus bus);
void handle_CAN_errors(void);
TRIPLE_Status setCAN_Termination(CAN_Bus bus, bool activated);
CAN_ErrorCounts getCANErrorCounts(CAN_Bus bus);
/* Protocol-error (ECR.CEL) counts the error ISR's register reads swallowed
 * since the last take, per bus (0..2). CEL clears on any read; the diagnostics
 * sampler owns the delta, so the ISR banks what its reads returned and this
 * hands the bank over, zeroing it. Saturates at 255. */
uint8_t CAN_TakeStolenErrorLog(uint8_t bus0);

// CAN Communication Layer Fuction Prototypes //
TRIPLE_Status send_FDmessage(CAN_Bus bus, bool is_extended_id,
                       uint32_t arbitration_id, uint8_t data_length,
                       uint8_t *data, bool brs);
TRIPLE_Status send_message(CAN_Bus bus, bool is_extended_id, uint32_t arbitration_id,
                     uint8_t data_length, uint8_t *data);
void onReceive(CAN_Message Message);

void trigger_CAN_RX(void);
void trigger_CAN_TX(void);
TRIPLE_Status enqueue_CAN_Message(CAN_MessageBuffer *buffer, CAN_Message *msg);
TRIPLE_Status dequeue_CAN_Message(CAN_MessageBuffer *buffer, CAN_Message *msg);
TRIPLE_Status Create_Circular_Buffer(uint16_t buffer_Identifier, bool is_Tx_Buffer,
                            uint16_t Size, bool overwritemode,
                            uint8_t *static_memory);
TRIPLE_Status add_to_CAN_RX_Queue(CAN_Bus bus, bool EXT_ID, uint32_t ID,
                            uint8_t data_length, uint8_t *rxData, bool BRS,
                            bool ESI, bool FD);

// Arithmatic Functions related to CAN Reception and Transmission //
float dbc_decode(const uint8_t *data, bool is_signed, bool is_big_endian,
                 uint16_t dbc_start_bit, uint8_t dbc_bit_length, float factor,
                 float offset, uint8_t decimal_places);
double dbc_decode_IEEE754(const uint8_t *data, bool is_double,
                          bool is_big_endian, uint16_t dbc_start_bit,
                          uint8_t dbc_bit_length, double factor, double offset,
                          uint8_t decimal_places);
int dbc_encode(uint8_t *data, size_t msg_data_length, datatype_t datatype,
               bool is_big_endian, float scaled_value, uint16_t dbc_start_bit,
               uint8_t dbc_bit_length, float factor, float offset);
int dbc_encode_ieee754(uint8_t *data, size_t msg_data_length, bool is_float,
                       bool is_big_endian, double scaled_value,
                       uint16_t dbc_start_bit, uint8_t dbc_bit_length);
float process_float_value(uint32_t value, uint32_t bitmask, bool is_signed,
                          float factor, float offset, int8_t decimal_places);
float process_ieee754(uint32_t value, uint32_t bitmask, float factor,
                      float offset, uint8_t decimal_places);
int32_t process_int_value(uint32_t value, uint32_t bitmask, bool is_signed,
                          int32_t factor, int32_t offset);
uint32_t process_unsigned_int_value(uint32_t value, uint32_t bitmask,
                                    uint32_t factor, uint32_t offset);
uint32_t process_raw_value(uint32_t value, uint32_t bitmask);
uint32_t prepare_output_signal(float value, uint8_t bitlength, bool is_signed,
                               float dbcFactor, float dbcOffset);
float roundfloat(float num, uint8_t decimal_places);
int32_t roundfloat_to_int32(float num, uint8_t decimal_places);
int32_t map_int(int32_t x, int32_t in_min, int32_t in_max, int32_t out_min,
                int32_t out_max);
float map_float(float x, float in_min, float in_max, float out_min,
                float out_max);
int32_t clamped_map_int(int32_t x, int32_t in_min, int32_t in_max,
                        int32_t out_min, int32_t out_max);
float clamped_map_float(float x, float in_min, float in_max, float out_min,
                        float out_max);
uint32_t frequency_Hz_to_period_10uS(uint32_t frequency_value);
uint32_t period_10uS_to_frequency_Hz(uint32_t period_value);

float getTimestamp();
// GPIO LED Function Prototypes //
void writeLED(gpio_LED led, bool high);
void toggleLED(gpio_LED led);

// CRC Checksum Calculation Function Prototypes //
uint8_t reflect8(uint8_t data);
uint16_t reflect16(uint16_t data);
uint32_t reflect32(uint32_t data);
uint8_t calculateCRC8(uint8_t *data, size_t length, uint8_t polynomial,
                      uint8_t crcInit, uint8_t finalXor, bool reflectInput,
                      bool reflectOutput);
uint16_t calculateCRC16(uint8_t *data, size_t length, uint16_t polynomial,
                        uint16_t crcInit, uint16_t finalXor, bool reflectInput,
                        bool reflectOutput);
uint32_t calculateCRC32(uint8_t *data, size_t length, uint32_t polynomial,
                        uint32_t crcInit, uint32_t finalXor, bool reflectInput,
                        bool reflectOutput);

// Security and Readout Protection Function Prototypes //
uint32_t getSerialNumber(void);
uint8_t getRDP(void);
uint8_t setRDP(bool on);

// Reading from Flash Function Prototypes //
uint8_t read_uint8_t_from_address(void *address);
int8_t read_int8_t_from_address(void *address);
uint16_t read_uint16_t_from_address(void *address);
int16_t read_int16_t_from_address(void *address);
uint32_t read_uint32_t_from_address(void *address);
int32_t read_int32_t_from_address(void *address);
float read_float_from_address(void *address);
char *read_char_array_from_address(const void *source, char *dest,
                                   size_t length);
void writeFlash(uint32_t page, uint8_t *Data, uint16_t dataSize);

// filtering functions //
float lowpass_filter_by_frequency(float basevalue, float newvalue,
                                  float time_constant, float frequency);
float lowpass_filter_by_timedelta(float basevalue, float newvalue,
                                  float time_constant, float last_timestamp,
                                  float current_timestamp);

// UART Debugger Function Prototypes //
void serialPrint(const char *str);
/* Returns whether the bytes were accepted; false means the double buffer was
 * full and they have been DROPPED. Callers that describe the bus to a host --
 * the monitor stream above all -- must mark the loss rather than let their
 * output quietly lose frames. */
bool serialWrite(const uint8_t *data, uint16_t length);
void tx_Serial_Comms();
void onSerialReceive(uint8_t *serialMessage);

char *format_CAN_message(const CAN_Message *msg, char *buffer, size_t buf_size);

#endif
