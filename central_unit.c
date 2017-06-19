/*
 * central_unit.c
 *
 *  Created on: 2017-06-01
 *      Author: Alessandro Martinelli
 */

/*
 * ASSUMPTIONS:
 * The initial state is: alarm deactivated, gate locked, garden lights turned off.
 * In general,
 * command 1 is for activating/deactivating the alarm
 * command 2 is for unlock and lock the gate
 * command 3 is for open and automatically close gate and door
 * command 4 is for obtaining the temperature mean value from the door node
 * command 5 is for obtaining the light value from the gate node
 */

/*
 * In order to send commands to nodes, the following code are used:
 * 1: alarm activation
 * 2: alarm deactivation
 * 3: auto opening and closing
 * 4: return measured value (temperature or light)
 * 5: gate unlocking
 * 6: gate locking
 *
 */


#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "dev/button-sensor.h"
#include "net/rime/rime.h"
#include "string.h" /* For strcmp() */
#include "dev/serial-line.h"
#include "stdlib.h"
#define MAX_COMMAND_ALLOWED 5
#define MAX_RETRANSMISSIONS 5
#define ALARM_ACTIVE			0x80	/* 1 if alarm is active */
#define AUTO_OPENING			0x40	/* 1 if automatic opening is occurring */
#define GATE_UNLOCKED			0x20	/* 1 if the gate is unlocked */
#define DOOR_NODE_ADDR_0		1
#define DOOR_NODE_ADDR_1		0
#define GATE_NODE_ADDR_0		2
#define GATE_NODE_ADDR_1		0
#define CU_NODE_ADDR_0			3
#define CU_NODE_ADDR_1			0
#define KITCHEN_NODE_ADDR_0		4
#define KITCHEN_NODE_ADDR_1		0


// #define STOP_GATE_AUTO_OPENING	0x10	/* 1 if the gate has communicated the end of the auto-opening procedure */
// #define STOP_DOOR_AUTO_OPENING	0x08	/* 1 if the door has communicated the end of the auto-opening procedure */


/*
 * Event used by the button process to communicate to the main process
 * that a valid command has been issued (the button has been pressed
 * a valid number of times
 */
static process_event_t user_command;

/*
 * Event used by the communication callback methods to forward the content
 * of a sensor-sent message to the main process.
 */
static process_event_t sensor_message;

/*
 * Variable used as an array of flags. Those flags store the state
 * of the system. They are used to perform operations in a consistent manner
 * and for printing only the correct subset of the commands as available commands.
 */
static uint8_t home_status;

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	// printf("[central unit]: runicast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
	process_post(NULL, sensor_message, (char*)packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	// printf("[central_unit]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	printf("runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static const struct broadcast_callbacks broadcast_call = {};
static struct broadcast_conn broadcast;
static const struct runicast_callbacks runicast_calls = {recv_runicast, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

void r_send(void* msg, int len, int rime_addr_0, int rime_addr_1){
	if(!runicast_is_transmitting(&runicast)) {
		linkaddr_t recv;
		packetbuf_copyfrom(msg, len);
		recv.u8[0] = rime_addr_0;
		recv.u8[1] = rime_addr_1;
		// printf("%u.%u: sending runicast to address %u.%u\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
		runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
	} else {
		// The previous transmission has not finished yet
		printf("It was not possible to issue the command. Try again later\n");
	}
}

/*
 * Prints the available commands in the moments it is called.
 * The list of available commands depend on the value of the
 * home_status variable, used to store the system state.
 */
void show_available_commands(){
	// To avoid concurrency problems (e.g. home_status changes value
	// while the comparisons are performed), his initial value is used.
	uint8_t current_status = home_status;
	if ((current_status & ALARM_ACTIVE) != 0){
		// Alarm active: only "deactive alarm" command is available.
		printf("\nAvailable comamnds are:\n"
				"1. ALARM DEACTIVATE\n\n");
	} else if((current_status & AUTO_OPENING) != 0){
		// Automatic opening and closing is active: you cannot directly lock/unlock the gate
		printf("\nAvailable comamnds are:\n"
				"1. ALARM ACTIVATE\n"
				"4. OBTAIN TEMPERATURE MEAN VALUE\n"
				"5. OBTAIN EXTERNAL LIGHT CURRENT VALUE\n"
				"CHANGE FIRE DETECTION THRESHOLD VIA SERIAL INPUT\n\n");
	} else if ((current_status & GATE_UNLOCKED) != 0){
		// Gate unlocked: we may issue the "GATE LOCK" command
		printf("\nAvailable comamnds are:\n"
				"1. ALARM ACTIVATE\n"
				"2. GATE LOCK\n"
				"3. OPEN AND AUTOMATICALLY CLOSE GATE AND DOOR\n"
				"4. OBTAIN TEMPERATURE MEAN VALUE\n"
				"5. OBTAIN EXTERNAL LIGHT CURRENT VALUE\n"
				"CHANGE FIRE DETECTION THRESHOLD VIA SERIAL INPUT\n\n");
	} else {
		// Gate locked: we may issue the "GATE UNLOCK" command
		printf("\nAvailable comamnds are:\n"
				"1. ALARM ACTIVATE\n"
				"2. GATE UNLOCK\n"
				"3. OPEN AND AUTOMATICALLY CLOSE GATE AND DOOR\n"
				"4. OBTAIN TEMPERATURE MEAN VALUE\n"
				"5. OBTAIN EXTERNAL LIGHT CURRENT VALUE\n"
				"CHANGE FIRE DETECTION THRESHOLD VIA SERIAL INPUT\n\n");
	}
}

/*---------------------------------------------------------------------------*/
PROCESS(central_unit_button_process, "Central Unit Button Process");
PROCESS(central_unit_main_process, "Central Unit Main Process");
AUTOSTART_PROCESSES(&central_unit_button_process, &central_unit_main_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(central_unit_button_process, ev, data)
{
	PROCESS_EXITHANDLER(broadcast_close(&broadcast));
	PROCESS_EXITHANDLER(runicast_close(&runicast));

	PROCESS_BEGIN();

	uint8_t ret;							// stores the return value when needed
	static uint8_t button_count;			// stores the number of button clicks
	static struct etimer button_timer;		// timer for waiting for a further button click after the first

	home_status = 0;
	user_command = process_alloc_event();
	sensor_message = process_alloc_event();
	broadcast_open(&broadcast, 129, &broadcast_call);
	runicast_open(&runicast, 144, &runicast_calls);
	SENSORS_ACTIVATE(button_sensor);

	show_available_commands();

	while(1){
		// Waiting for the first button click
		button_count=0;
		PROCESS_WAIT_EVENT_UNTIL(ev == sensors_event && data == &button_sensor);
		// First button click, the process wait for another click within 4 seconds
		button_count++;
		etimer_set(&button_timer,CLOCK_SECOND*4);
		while(1){
			// Wait for the button to be press further or for the timer to elapse
			PROCESS_WAIT_EVENT();
			if (ev == sensors_event && data == &button_sensor){
				// The button has been pressed another time
				button_count++;
				if (button_count > MAX_COMMAND_ALLOWED){
					// The button has been pressed more than MAX_COMMAND_ALLOWED times,
					// thus the timer must be stopped (the counter is reset in the main loop)
					printf("Invalid command\n");
					etimer_stop(&button_timer);
					show_available_commands();
					break;
				} else {
					// The number of button clicks is still valid. Wait again.
					etimer_restart(&button_timer);
				}
			} else if(ev == PROCESS_EVENT_TIMER && etimer_expired(&button_timer)){
				// Timer elapsed. The number of button clicks can be assumed to be
				// valid. The value is sent to the main process for further elaboration
				ret = process_post(&central_unit_main_process, user_command, (void*)(int)button_count);
				if (ret != 0){
					// Event queue full
					printf("It was not possible to issue the command. Try again later\n");
					show_available_commands();
				}
				break;
			}
		}

	}

	SENSORS_DEACTIVATE(button_sensor);
	PROCESS_END();
	return 0;
}



PROCESS_THREAD(central_unit_main_process, ev, data)
{
	PROCESS_BEGIN();

	static uint8_t button_count;	// Stores the number of button clicks detected in the button process
	uint8_t out_command;			// Stores the command to be sent to some node.
	char in_msg[8];
	char out_msg[8];

	while(1){
		// Wait for either
		// 1) a command (the button has been clicked some times);
		// 2) a message from a sensor node;
		// 3) an input from the serial line
		PROCESS_WAIT_EVENT();
		if(ev == user_command){
			// An user command has been received
			button_count = (uint8_t)(int)data;
			switch(button_count){
				case 1:
					// Activate/deactivate alarm command, it is always possible to issue it.
					if ((home_status & ALARM_ACTIVE) == 0){
						// The alarm is off, it has to be turned on.
						home_status |= ALARM_ACTIVE;
						out_command = 1;
						packetbuf_copyfrom((void*)&out_command, 1);
					    broadcast_send(&broadcast);
					} else {
						// The alarm is on, it has to be turned off.
						home_status &= ~ALARM_ACTIVE;
						out_command = 2;
						packetbuf_copyfrom((void*)&out_command, 1);
					    broadcast_send(&broadcast);
					}
					show_available_commands();
					break;
				case 2:
					// Lock/unlock command. It is possible to issue it only if
					// 1.the alarm is deactivated, 2.the automatic opening-closing procedure is not active.
					if (((home_status & ALARM_ACTIVE) != 0) || ((home_status & AUTO_OPENING) != 0)){
						printf("Invalid command\n");
					} else {
						// It is possible to issue the command
						if ((home_status & GATE_UNLOCKED) == 0){
							// the gate is locked, thus it has to be unlocked
							home_status |= GATE_UNLOCKED;
							out_command = 5;
							r_send((void*)&out_command, sizeof(uint8_t), GATE_NODE_ADDR_0, GATE_NODE_ADDR_1);
						} else {
							// the gate is unlocked, thus it has to be locked
							home_status &= ~GATE_UNLOCKED;
							out_command = 6;
							r_send((void*)&out_command, sizeof(uint8_t), GATE_NODE_ADDR_0, GATE_NODE_ADDR_1);
						}
					}
					show_available_commands();
					break;
				case 3:
					// Auto opening and closing of gate and door command. It is possible to issue it only if
					// 1.the alarm is deactivated, 2.the automatic opening-closing procedure is not already active.
					if (((home_status & ALARM_ACTIVE) != 0) || ((home_status & AUTO_OPENING) != 0)){
						printf("Invalid command\n");
					} else {
						// It is possible to issue the command.
						home_status |= AUTO_OPENING;
						out_command = 3;
						packetbuf_copyfrom((void*)&out_command, 1);
					    broadcast_send(&broadcast);
					}
					show_available_commands();
					break;
				case 4:
					// Temperature mean command.  It is possible to issue it only if
					// the alarm is deactivated,
					if ((home_status & ALARM_ACTIVE) != 0){
						printf("Invalid command\n");
					} else {
						// It is possible to issue the command
						out_command = 4;
						r_send((void*)&out_command, sizeof(uint8_t), DOOR_NODE_ADDR_0, DOOR_NODE_ADDR_1);
					}
					show_available_commands();
					break;
				case 5:
					// External light command. It is possible to issue it only if
					// the alarm is deactivated,
					if ((home_status & ALARM_ACTIVE) != 0){
						printf("Invalid command\n");
					} else {
						// It is possible to issue the command
						out_command = 4;
						r_send((void*)&out_command, sizeof(uint8_t), GATE_NODE_ADDR_0, GATE_NODE_ADDR_1);
					}
					show_available_commands();
					break;
				default:
					break;
			}
		} else if (ev == sensor_message){
			// A message from a sensor node has been received
			strcpy(in_msg, (char*)data);
			if (strcmp(in_msg, "stop") == 0){
				// Auto opening stop message.
				// As soon as we receive the 'stop' message from even one
				// between the gate node and the door node, we assume that
				// the auto-opening procedure has terminated for both of them.
				home_status &= ~AUTO_OPENING;
				show_available_commands();
			} else if((strncmp(in_msg, "li", 2)) == 0){
				// External light value message. We just show the received value
				char value[5];
				uint8_t initial_index = 2;
				uint8_t len = strlen(in_msg) - initial_index;
				strncpy(value, in_msg+initial_index, len);
				value[len] = '\0';
				printf("External light is %s\n", value);
			} else if((strncmp(in_msg, "tem", 3)) == 0){
				// Temperature message. We just show the received value
				char value[5];
				uint8_t initial_index = 3;
				uint8_t len = strlen(in_msg) - initial_index;
				strncpy(value, in_msg+initial_index, len);
				value[len] = '\0';
				printf("Temperature mean value is %s\n", value);
			} else if((strncmp(in_msg, "fi", 2)) == 0){
				// Fire detected message. We print a message, send the alarm and
				// issue the command to turn off the camera
				printf("A FIRE HAS BEEN DETECTED! TEMPERATURE %s\n", in_msg+2);

				home_status |= ALARM_ACTIVE;
				out_command = 1;
				packetbuf_copyfrom((void*)&out_command, 1);
			    broadcast_send(&broadcast);
			    show_available_commands();

				strcpy(out_msg, "camoff");
				r_send((void*)&out_msg, strlen(out_msg) + 1, KITCHEN_NODE_ADDR_0, KITCHEN_NODE_ADDR_1);
			}
		} else if(ev == serial_line_event_message){
			// An input from the serial line has arrived. It is possible
			// to issue this command only if the alarm is deactivated.
			if ((home_status & ALARM_ACTIVE) != 0){
				printf("Invalid command\n");
			} else {
				// It is possible to issue the command
				strcpy(out_msg, "th");
				strcat(out_msg, (char*)data);
				r_send((void*)&out_msg, strlen(out_msg) + 1, KITCHEN_NODE_ADDR_0, KITCHEN_NODE_ADDR_1);
			}
		}
	}

	PROCESS_END();
	return 0;
}

