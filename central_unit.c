// TODO: fill
/*
 * central_unit.c
 *
 *  Created on: 2017-06-01
 *    Modified: 2017-06-03
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

/* TODO:
 * - Check if it is necessary to deactivate button_sensor each time.
 * - Togliere tutte le stampe di debug
 * - rimuovere i file project_conf.h, se alla fine non sono serviti
 */


#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "dev/button-sensor.h"
#include "net/rime/rime.h"
#include "string.h" /* For strcmp() */
#define MAX_COMMAND_ALLOWED 5
#define MAX_RETRANSMISSIONS 5
#define PROCESS_EVENT_BUTTON 138
#define ALARM_ACTIVE			0x80	/* 1 if alarm is active */
#define AUTO_OPENING			0x40	/* 1 if automatic opening is occurring */
#define GATE_UNLOCKED			0x20	/* 1 if the gate is unlocked */
// #define STOP_GATE_AUTO_OPENING	0x10	/* 1 if the gate has communicated the end of the auto-opening procedure */
// #define STOP_DOOR_AUTO_OPENING	0x08	/* 1 if the door has communicated the end of the auto-opening procedure */


static process_event_t user_command;
static process_event_t sensor_message;
static uint8_t home_status;

static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from){
  printf("[central_unit]: broadcast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
}

static void broadcast_sent(struct broadcast_conn *c, int status, int num_tx){
  printf("[central_unit]: broadcast message sent. Status %d. For this packet, this is transmission number %d\n", status, num_tx);
}

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	printf("[central unit]: runicast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
	process_post(NULL, sensor_message, (char *)packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("[central_unit]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("[central_unit]: runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static const struct broadcast_callbacks broadcast_call = {broadcast_recv, broadcast_sent}; //Be careful to the order
static struct broadcast_conn broadcast;
static const struct runicast_callbacks runicast_calls = {recv_runicast, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

void show_available_commands(){
	uint8_t current_status = home_status;
	if ((current_status & ALARM_ACTIVE) != 0){
		// Alarm active: only "deactive alarm" command is available.
		printf("\n[Serial Port Output]: Commands available are:\n"
				"1. ALARM DEACTIVATE\n");
	} else if((current_status & AUTO_OPENING) != 0){
		// Automatic opening and closing is active: you cannot directly lock/unlock the gate
		printf("\n[Serial Port Output]: Commands available are:\n"
				"1. ALARM ACTIVATE\n"
				"4. OBTAIN TEMPERATURE MEAN VALUE\n"
				"5. OBTAIN EXTERNAL LIGHT CURRENT VALUE\n");
	} else if ((current_status & GATE_UNLOCKED) != 0){
		// Gate unlocked: we may issue the "GATE LOCK" command
		printf("\n[Serial Port Output]: Commands available are:\n"
				"1. ALARM ACTIVATE\n"
				"2. GATE LOCK\n"
				"3. OPEN AND AUTOMATICALLY CLOSE GATE AND DOOR\n"
				"4. OBTAIN TEMPERATURE MEAN VALUE\n"
				"5. OBTAIN EXTERNAL LIGHT CURRENT VALUE\n");
	} else {
		// Gate locked: we may issue the "GATE UNLOCK" command
		printf("\n[Serial Port Output]: Commands available are:\n"
				"1. ALARM ACTIVATE\n"
				"2. GATE UNLOCK\n"
				"3. OPEN AND AUTOMATICALLY CLOSE GATE AND DOOR\n"
				"4. OBTAIN TEMPERATURE MEAN VALUE\n"
				"5. OBTAIN EXTERNAL LIGHT CURRENT VALUE\n");
	}
}

/*
uint8_t hash(char* data){
	if (strcmp(data, "auto opening stop") == 0){
		return 1;
	}
	else if(strncmp(data, "External") == 0){
		return 2;
	}
	/*
	else if(strcmp(data, "gate unlocking") == 0){
		return 3;
	} else if(strcmp(data, "gate locking") == 0){
		return 4;
	} else if(strcmp(data, "auto opening") == 0){
		return 5;
	} else if(strcmp(data, "external light") == 0){
		return 6;
	}

	else {
		return 0;
	}
}
*/

/*---------------------------------------------------------------------------*/
PROCESS(central_unit_button_process, "Central Unit Button Process");
PROCESS(central_unit_main_process, "Central Unit Main Process");
AUTOSTART_PROCESSES(&central_unit_button_process, &central_unit_main_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(central_unit_button_process, ev, data)
{
	static uint8_t ret;
	static uint8_t button_count;
	static struct etimer button_timer;
	PROCESS_EXITHANDLER(broadcast_close(&broadcast));
	PROCESS_EXITHANDLER(runicast_close(&runicast));

	PROCESS_BEGIN();

	// Initializations
	user_command = process_alloc_event();
	sensor_message = process_alloc_event();
	home_status = 0;
	broadcast_open(&broadcast, 129, &broadcast_call);
	runicast_open(&runicast, 144, &runicast_calls);
	SENSORS_ACTIVATE(button_sensor);
	show_available_commands();

	while(1){
		// TODO printf("[centra_unit]: waiting for a button!\n");
		// Initialization and waiting for the first button click
		button_count=0;
		PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_BUTTON);
		// TODO printf("[centra_unit]: first button press!\n");
		button_count++;
		etimer_set(&button_timer,CLOCK_SECOND*4);
		while(1){
			// Wait for the button to be press further or for the timer to elapse
			PROCESS_WAIT_EVENT();
			if (ev == PROCESS_EVENT_BUTTON){
				// TODO printf("[centra_unit]: another button press!\n");
				// The button has been pressed another time
				button_count++;
				if (button_count > MAX_COMMAND_ALLOWED){
					// The button has been pressed more than MAX_COMMAND_ALLOWED times,
					// thus the count is stopped
					printf("[Serial Port Output]: Invalid command\n");
					etimer_stop(&button_timer);
					show_available_commands();
					break;
				} else {
					// The number of clicks of the button is still valid. We wait
					// for another 4 seconds for button clicks.
					etimer_restart(&button_timer);
				}
			} else if(etimer_expired(&button_timer)){
				printf("[centra_unit]: the button has been pressed %d times!\n", button_count);
				// Timer elapsed. The number of button clicks can be assumed to be
				// valid, thus this value is sent to the main process for
				// further elaboration
				ret = process_post(&central_unit_main_process, user_command, (void*)button_count);
				if (ret != 0){
					// Event queue full
					printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
					show_available_commands();
				}
				//TODO: printf("[cu_button]: command %d sent, %d is returned\n", button_count, ret);
				break;

			}
		}

	}
	//TODO: questa parte qui sotto non viene mai raggiunta, quindi potrei anche toglierla
	printf("[Serial Port Output]: Ending...\n");
	SENSORS_DEACTIVATE(button_sensor);

	PROCESS_END();
	return 0;
}



PROCESS_THREAD(central_unit_main_process, ev, data)
{
	PROCESS_BEGIN();

	static uint8_t command;
	// TODO static uint8_t message_type;
	// TODO char msg;

	while(1){
		PROCESS_WAIT_EVENT();
		//printf("[central_unit_main]: received event %d, data %s or %d\n", ev, data, data);
		if(ev == user_command){
			command = (uint32_t)data;
			// TODO: printf("[cu_main]: received data %d, command %d\n", data, command);

			switch(command){
			//TODO: I corpi di questi case forse sarebbe il caso di raggrupparli in funzioni, perlomeno quelli complessi.
				case 1:
					printf("[cu_main]: handling of command 1\n");
					// This is the activate/deactivate alarm, it is always possible to issue it.
					if ((home_status & ALARM_ACTIVE) == 0){
						// The alarm is off, it has to be turned on.
						printf("[cu_main]: alarm activation\n");
						home_status |= ALARM_ACTIVE;
					    packetbuf_copyfrom("alarm activation", 17);
					    broadcast_send(&broadcast);
						// TODO: broadcast message
					} else {
						// The alarm is on, it has to be turned off.
						printf("[cu_main]: alarm deactivation\n");
						home_status &= ~ALARM_ACTIVE;
					    packetbuf_copyfrom("alarm deactivation", 19);
					    broadcast_send(&broadcast);
						// TODO: broadcast message
					}
					show_available_commands();
					break;
				case 2:
					printf("[cu_main]: handling of command 2\n");
					// This is the lock/unlock command. It is possible to issue it only if
					// 1. The alarm is deactivated 2. the automatic open-close is not active.
					if (((home_status & ALARM_ACTIVE) != 0) || ((home_status & AUTO_OPENING) != 0)){
						printf("[Serial Port Output]: Invalid command\n");
					} else {
						if ((home_status & GATE_UNLOCKED) == 0){
							printf("[cu_main]: gate unlocking\n");
							home_status |= GATE_UNLOCKED;
							if(!runicast_is_transmitting(&runicast)) {
							      linkaddr_t recv;
							      packetbuf_copyfrom("gate unlocking", 15);
							      recv.u8[0] = 3;
								  recv.u8[1] = 0;
							      printf("%u.%u: sending runicast to address %u.%u\n",
							    		  linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
							      runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
							} else {
								// The previous transmission has not finished yet
								printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
							}
						} else {
							printf("[cu_main]: gate locking\n");
							home_status &= ~GATE_UNLOCKED;
							if(!runicast_is_transmitting(&runicast)) {
								linkaddr_t recv;
								packetbuf_copyfrom("gate locking", 13);
								recv.u8[0] = 3;
								recv.u8[1] = 0;
								printf("%u.%u: sending runicast to address %u.%u\n",
										linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
								runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
							} else {
								// The previous transmission has not finished yet
								printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
							}
						}
					}
					show_available_commands();
					break;
				case 3:
					printf("[cu_main]: handling of command 3\n");
					if (((home_status & ALARM_ACTIVE) != 0) || ((home_status & AUTO_OPENING) != 0)){
						printf("[Serial Port Output]: Invalid command\n");
					} else {
						home_status |= AUTO_OPENING;
					    packetbuf_copyfrom("auto opening", 13);
					    broadcast_send(&broadcast);
						//TODO: tale bit verra' riportato ad 1 nella gestione del messaggio di risposta,
						// e solo quando questo e' arrivato da entrambi.
						// Questo si puo' fare con i bit 4 e 5 di home_status.
						// Inoltre in tal caso possiamo far partire un timer di ad esempio 30 secondi: quando scade,
						// possiamo assumere che gate e door si sno richiusi
						//TODO: qui stara' al nodo cambiare momentaneamente i colori (verde e rosso)
						// e poi rimetterli a posto, non c'e' bisogno che la CU ne tenga traccia.
					}
					show_available_commands();
					break;
				case 4:
					printf("[cu_main]: handling of command 4\n");
					if ((home_status & ALARM_ACTIVE) != 0){
						printf("[Serial Port Output]: Invalid command\n");
					} else {
						if(!runicast_is_transmitting(&runicast)) {
							linkaddr_t recv;
							packetbuf_copyfrom("temperature mean", 17);
							recv.u8[0] = 2;
							recv.u8[1] = 0;
							printf("%u.%u: sending runicast to address %u.%u\n",
									linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
							runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
						} else {
							// The previous transmission has not finished yet
							printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
						}
					}
					show_available_commands();
					break;
				case 5:
					printf("[cu_main]: handling of command 5\n");
					if ((home_status & ALARM_ACTIVE) != 0){
						printf("[Serial Port Output]: Invalid command\n");
					} else {
						if(!runicast_is_transmitting(&runicast)) {
							linkaddr_t recv;
							packetbuf_copyfrom("external light", 15);
							recv.u8[0] = 3;
							recv.u8[1] = 0;
							printf("%u.%u: sending runicast to address %u.%u\n",
									linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
							runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
						} else {
							// The previous transmission has not finished yet
							printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
						}
					}
					show_available_commands();
					break;
			}
		} else if (ev == sensor_message){
			// TODO printf("[central unit]: got a sensor message! data is %s\n", data);
			if (strcmp(data, "auto opening stop") == 0){
				/* auto opening stop message */
				// We suppose that, as soon as we receive the
				// 'auto opening stop' message from even only
				// one of the two node (gate and door nodes)
				// we assume that the auto-opening procedure
				// has terminated for both of them.
				// TODO printf("[central unit]: Ah, so the auto opening procedure is finished?\n");
				home_status &= ~AUTO_OPENING;
				show_available_commands();
			} else if((strncmp(data, "light", 5)) == 0){
				/* auto opening stop message */
				char value[5];
				uint8_t initial_index = 5;
				uint8_t len = strlen(data) - initial_index;
				strncpy(value, data+initial_index, len);
				value[len] = '\0';
				printf("[Serial Port Output]: External light is %s\n", value);
				// TODO: forse qui ci va lo "show available commands"?
			} else if((strncmp(data, "temp", 4)) == 0){
				/* auto opening stop message */
				char value[5];
				uint8_t initial_index = 4;
				uint8_t len = strlen(data) - initial_index;
				strncpy(value, data+initial_index, len);
				value[len] = '\0';
				printf("[Serial Port Output]: Temperature mean value is %s\n", value);
				// TODO: forse qui ci va lo "show available commands"?
			}
		}
	}

	//TODO: remove this code if it is still here
	//static struct etimer button_timer;
	//etimer_set(&button_timer,CLOCK_SECOND*10);
	//PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&button_timer));
	//printf("[DEBUG cu_main_process]: Attesa finita, ora ricevo eventi asincroni\n");

	PROCESS_END();

	return 0;
}

