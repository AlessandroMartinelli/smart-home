// TODO: fill
/*
 * gate_node.c
 *
 *  Created on: 2017-06-03
 *    Modified:
 *      Author: Alessandro Martinelli
 */

/*
 * TODO:
 * - Mi e' venuto in mente, inoltre, che per fare le cose ammodo dovrei, in tutti i nodi, non solo controllare
 * che ev == TIMER_EVENT (o quel che e') ma anche che sia scaduto il timer che penso io, altrimenti magari scade
 * un altro timer, di sistema o altro, boh. Insomma puliafito nelle lezioni la controllava questa condizione,
 * quindi forse ha senso guardarla.
 *
 */

/*
 * ASSUMPTIONS:
 * The initial state is: alarm deactivated, gate locked, garden lights turned off.
 *
 */


#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "net/rime/rime.h"
#include "string.h"
#include "dev/leds.h"
#include "dev/light-sensor.h" // TODO: only in gate node

#define ALARM_ACTIVE		0x80	/* 1 if alarm is active */
#define AUTO_OPENING		0x40	/* 1 if automatic opening is occurring */
#define GATE_UNLOCKED		0x20	/* 1 if the gate is unlocked */
#define MAX_RETRANSMISSIONS 5

static process_event_t message_from_central_unit;
static process_event_t alarm_blink;
static process_event_t opening_blink;
static process_event_t opening_blink_stop;

static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from){
	printf("[gate node]: broadcast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
	process_post(NULL, message_from_central_unit, (char *)packetbuf_dataptr());
}

static void broadcast_sent(struct broadcast_conn *c, int status, int num_tx){
	printf("[gate node]: broadcast message sent. Status %d. For this packet, this is transmission number %d\n", status, num_tx);
}

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	printf("[gate node]: runicast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
	process_post(NULL, message_from_central_unit, (char *)packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("[gate node]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("[gate node]: runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static const struct broadcast_callbacks broadcast_call = {broadcast_recv, broadcast_sent};
static struct broadcast_conn broadcast;
static const struct runicast_callbacks runicast_calls = {recv_runicast, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

uint8_t hash(char* data){
	if (strcmp(data, "alarm activation") == 0){
		return 1;
	} else if(strcmp(data, "alarm deactivation") == 0){
		return 2;
	} else if(strcmp(data, "gate unlocking") == 0){
		return 3;
	} else if(strcmp(data, "gate locking") == 0){
		return 4;
	} else if(strcmp(data, "auto opening") == 0){
		return 5;
	} else if(strcmp(data, "external light") == 0){
		return 6;
	} else {
		return 0;
	}
}

static uint8_t home_status; //TODO se faccio un solo processo, potrei anche definirla internamente ad esso.

/*---------------------------------------------------------------------------*/
PROCESS(gate_node_main_process, "Gate Node Main Process");
PROCESS(gate_node_alarm_blink_process, "Gate Node Alarm Led Process");
PROCESS(gate_node_opening_blink_process, "Gate Node Opening Led Process");
AUTOSTART_PROCESSES(&gate_node_main_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(gate_node_main_process, ev, data)
{
	PROCESS_EXITHANDLER(broadcast_close(&broadcast));
	PROCESS_EXITHANDLER(runicast_close(&runicast));

	PROCESS_BEGIN();

	uint8_t command = 0;
	home_status = 0;
	// This customized message is declared here even if it is used in the
	// broadcast received function. But it's ok, since the broadcast_open function
	// is called only after the custom event initialization.
	message_from_central_unit = process_alloc_event();
	alarm_blink = process_alloc_event();
	opening_blink = process_alloc_event();
	opening_blink_stop = process_alloc_event();
	broadcast_open(&broadcast, 129, &broadcast_call);
	runicast_open(&runicast, 144, &runicast_calls);
	// At the beginning, the gate is locked.
	leds_off(LEDS_GREEN);
	leds_on(LEDS_RED);
	leds_off(LEDS_BLUE);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == message_from_central_unit){
			command = hash((char*)data);
			switch(command){
				case 1:
					/* alarm activation command */
					if((home_status & ALARM_ACTIVE) == 0){
						printf("[gate node]: alarm activation handling\n");
						home_status |= ALARM_ACTIVE;
						process_start(&gate_node_alarm_blink_process, NULL);
						//process_post(&gate_node_alarm_blink_process, PROCESS_EVENT_INIT, NULL);
					}
					break;
				case 2:
					/* alarm deactivation command */
					if((home_status & ALARM_ACTIVE) != 0){
						printf("[gate node]: alarm deactivation handling\n");
						home_status &= ~(ALARM_ACTIVE);
						// leds has to return in their previous state
						if((home_status & GATE_UNLOCKED) != 0){
							leds_on(LEDS_GREEN);
							leds_off(LEDS_RED);
							leds_off(LEDS_BLUE);
						} else {
							leds_off(LEDS_GREEN);
							leds_on(LEDS_RED);
							leds_off(LEDS_BLUE);
						}
						process_exit(&gate_node_alarm_blink_process);
					}
					break;
				case 3:
					/* gate unlocking command */
					if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & AUTO_OPENING) == 0)){
						home_status |= GATE_UNLOCKED;
						leds_on(LEDS_GREEN);
						leds_off(LEDS_RED);
					}
					break;
				case 4:
					/* gate locking command */
					if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & AUTO_OPENING) == 0)){
						home_status &= ~GATE_UNLOCKED;
						leds_off(LEDS_GREEN);
						leds_on(LEDS_RED);
					}
					break;
				case 5:
					/* auto opening command */
					if((home_status & ALARM_ACTIVE) == 0){
						printf("[gate node]: auto opening handling\n");
						home_status |= AUTO_OPENING;
						process_start(&gate_node_opening_blink_process, NULL);
					}
					break;
				case 6:
					/* external temperature command */
					if((home_status & ALARM_ACTIVE) == 0){
						SENSORS_ACTIVATE(light_sensor);
						static int light;
						static char msg[10];
						printf("[gate node]: rev.7\n");
						light = (10*light_sensor.value(LIGHT_SENSOR_PHOTOSYNTHETIC)/7);
						printf("[gate node]: sampled temperature is %d\n", light);
						SENSORS_DEACTIVATE(light_sensor);
						sprintf(msg, "light%d", light);
						//printf("[gate node]: sampled light string is %s, length %d\n", value, strlen(value));

						//msg = strcat("temp ", value);
						printf("[gate node]: msg is %s, strlen is %d\n", msg, strlen(msg));
						if(!runicast_is_transmitting(&runicast)) {
							linkaddr_t recv;
							packetbuf_copyfrom(msg, strlen(msg)+1);
							recv.u8[0] = 1;
							recv.u8[1] = 0;
							printf("[gate node]: %u.%u: sending runicast to address %u.%u\n",
								  linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
							runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
						} else {
							// The previous transmission has not finished yet
							printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
						}

					}
					break;
				default:
					break;
			}
			//printf("[gate node]: message from central unit, content is %s\n", data);
		} else if(ev == alarm_blink){
			// It may happen that gate_node_alarm_led_process has sent the alarm_blink
			// event just before the "deactivate alarm" was issued. Here this
			// possible race is handled .
			if((home_status & ALARM_ACTIVE) != 0){
				// If all led are on, all of them have to be turned off;
				// otherwise turn them on (even if some are on and some are off,
				// since alarm has the maximum "priority".
				if(leds_get() != 0){
					leds_off(LEDS_ALL);
				} else {
					leds_on(LEDS_ALL);
				}
			}
		} else if(ev == opening_blink){
			if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & AUTO_OPENING) != 0)){
				if((home_status & GATE_UNLOCKED) == 0){
					// If the gate was locked, we temporarily unlock it.
					// This is done by means of the leds.
					leds_on(LEDS_GREEN);
					leds_off(LEDS_RED);
				}
				if(((int)data%2) != 0){
					leds_on(LEDS_BLUE);
				} else {
					leds_off(LEDS_BLUE);
				}
			}
		} else if(ev == opening_blink_stop){
			home_status &= ~AUTO_OPENING;
			if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & GATE_UNLOCKED) == 0)){
				leds_off(LEDS_GREEN);
				leds_on(LEDS_RED);
			}
			if(!runicast_is_transmitting(&runicast)) {
				linkaddr_t recv;
				packetbuf_copyfrom("auto opening stop", 18);
				recv.u8[0] = 1;
				recv.u8[1] = 0;
				printf("[gate node]: %u.%u: sending runicast to address %u.%u\n",
					  linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
				runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
			} else {
				// The previous transmission has not finished yet
				printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
			}

			// There should be no need of turning off the blue led:
			// if the opening process was not interrupted, the final state is off;
			// if the opening process was interrupted while blue on, blu will be turned off by alarm deactivation;
			// if the opening process was interrupted while blue off, nothing to worry about
		}
		else {
			printf("[gate node]: ev is %d\n", ev);
		}
	}
	PROCESS_END();
	return 0;
}

PROCESS_THREAD(gate_node_alarm_blink_process, ev, data)
{
	PROCESS_BEGIN();
	printf("[gate node]: Hello from alarm blink process\n");
	static struct etimer blink_timer;
	etimer_set(&blink_timer,CLOCK_SECOND*2);

	while(1){
		PROCESS_WAIT_EVENT();
		if(etimer_expired(&blink_timer)){
			process_post(&gate_node_main_process, alarm_blink, NULL);
			etimer_reset(&blink_timer);
		}
	}
	PROCESS_END();
	return 0;
}


PROCESS_THREAD(gate_node_opening_blink_process, ev, data)
{
	PROCESS_BEGIN();
	printf("[gate node]: Hello from opening blink process\n");
	static uint8_t blinked;
	static struct etimer blink_timer;
	etimer_set(&blink_timer,CLOCK_SECOND*2);

	for(blinked = 1; blinked < 9; blinked++){
		PROCESS_WAIT_EVENT();
		if(etimer_expired(&blink_timer)){
			process_post(&gate_node_main_process, opening_blink, (void*)blinked);
			if(blinked != 8){
				etimer_reset(&blink_timer);
			}
		}
	}
	process_post(&gate_node_main_process, opening_blink_stop, NULL);
	printf("[gate node]: Goodbye from opening blink process\n");
	PROCESS_END();
	return 0;
}
