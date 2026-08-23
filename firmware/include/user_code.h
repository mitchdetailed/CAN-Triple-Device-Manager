/*
 * user_code.h
 */

/* Includes Declarations */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defines Declarations */
#ifndef INC_USER_CODE_H_
#define INC_USER_CODE_H_



#define HALTECH_CAN CAN_3 //



/* v2: 8 KB per double buffer (was 32 KB). At 7.37 Mbaud a full buffer drains
 * in ~11 ms; the old size cost 64 KB of the 128 KB RAM. */
#define UART_MSG_BUFFER_SIZE 8192 /* UART String Buffer Size*/
// #define UART_DEBUG_BAUDRATE       115200      /* Default UART Debug
// Baudrate*/ #define UART_DEBUG_BAUDRATE       921600      /* Teleplot Max UART
// Debug Baudrate?*/
#define UART_DEBUG_BAUDRATE 7372800 /* Maximum UART Debug Baudrate*/

/* Function Prototypes */

void events_Startup(void);
void events_2000Hz(void);
void events_1000Hz(void);
void events_500Hz(void);
void events_200Hz(void);
/* Takes the number of 100 Hz periods that have elapsed, unlike every other
 * group here, because it is the only one driving anything that measures TIME
 * rather than counting visits. See x100Hz_pending in main.c. */
void events_100Hz(uint16_t ticks);
void events_50Hz(void);
void events_20Hz(void);
void events_10Hz(void);
void events_5Hz(void);
void events_2Hz(void);
void events_1Hz(void);
void events_Shutdown(void);

#endif /* INC_USER_CODE_H_ */
