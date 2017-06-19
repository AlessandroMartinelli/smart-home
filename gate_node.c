/*
 * gate_node.c
 *
 *  Created on: 2017-06-03
 *      Author: Alessandro Martinelli
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
#define CU_NODE_ADDR_0			3
#define CU_NODE_ADDR_1			0

static process_event_t message_from_central_unit;
static process_event_t alarm_blink;
static process_event_t opening_blink;
static process_event_t opening_blink_stop;

static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from){
	// printf("[gate node]: broadcast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
	process_post(NULL, message_from_central_unit, (char *)packetbuf_dataptr());
}

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	// printf("[gate node]: runicast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());

	// Since the processes have not been declared yet, the message is sent to all processes
	process_post(NULL, message_from_central_unit, (char*)packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	// printf("[gate node]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	printf("[gate node]: runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;
static const struct runicast_callbacks runicast_calls = {recv_runicast, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

/*
 * Variable used as an array of flags. Those flags store the state
 * of the system. They are used to perform operations in a consistent manner
 */
static uint8_t home_status;

void r_send_to_cu(void* msg){
	if(!runicast_is_transmitting(&runicast)) {
		linkaddr_t recv;
		packetbuf_copyfrom(msg, strlen(msg) + 1);
		recv.u8[0] = CU_NODE_ADDR_0;
		recv.u8[1] = CU_NODE_ADDR_1;
		// printf("%u.%u: sending runicast to address %u.%u\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
		runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
	} else {
		// The previous transmission has not finished yet
		printf("[central unit]: It was not possible to issue the command. Try again later\n");
	}
}

int obtain_light(){
	SENSORS_ACTIVATE(light_sensor);
	int light = ((10*light_sensor.value(LIGHT_SENSOR_PHOTOSYNTHETIC))/7);
	printf("[gate node]: sampled light is %d\n", light);
	SENSORS_DEACTIVATE(light_sensor);
	return light;
}

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

	uint8_t command;			// Stores the command received from the central unit
	char out_msg[10];			// Used to store message to send to the central unit

	command = 0;
	home_status = 0;

	// This customized message is declared here even if it is used in the
	// broadcast received function. But it's ok, since the broadcast_open function
	// is called only after this custom event initialization.
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
		// We wait for either
		// 1) a command from the central unit
		// 2) the message of changing the leds in the alarm way
		// 3) the message of changing the leds in the automatic opening way
		// 4) the message of stop to change the leds in the automatic opening way
		if(ev == message_from_central_unit){
			// A command from the central unit has arrived
			command = *(uint8_t*)data;
			switch(command){
				case 1:
					// Alarm activation command. We start a process in charge of
					// sending us a alarm_blink message periodically. We will react
					// to that message by setting the leds in the appropriate way.
					if((home_status & ALARM_ACTIVE) == 0){
						home_status |= ALARM_ACTIVE;
						process_start(&gate_node_alarm_blink_process, NULL);
					}
					break;
				case 2:
					// Alarm deactivation command. Leds has to go back
					// inthe state they were before the alarm activation
					if((home_status & ALARM_ACTIVE) != 0){
						home_status &= ~(ALARM_ACTIVE);
						if((home_status & GATE_UNLOCKED) != 0){
							leds_on(LEDS_GREEN);
							leds_off(LEDS_RED);
							leds_off(LEDS_BLUE);
						} else {
							leds_off(LEDS_GREEN);
							leds_on(LEDS_RED);
							leds_off(LEDS_BLUE);
						}
						// We stop the process in charge of sending alarm_blink messages
						process_exit(&gate_node_alarm_blink_process);
					}
					break;
				case 3:
					// Auto opening command. We start a process in charge of
					// sending us an opening_blink message periodically. We will react
					// to that message by setting the leds in the appropriate way.
					if(((home_status & ALARM_ACTIVE) == 0) || ((home_status & AUTO_OPENING) == 0)){
						home_status |= AUTO_OPENING;
						process_start(&gate_node_opening_blink_process, NULL);
					}
					break;
				case 4:
					/* external light command */
					if((home_status & ALARM_ACTIVE) == 0){
						int light = obtain_light();
						sprintf(out_msg, "li%d", light);
						r_send_to_cu(out_msg);
					}
					break;
				case 5:
					/* gate unlock command */
					if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & AUTO_OPENING) == 0)){
						home_status |= GATE_UNLOCKED;
						leds_on(LEDS_GREEN);
						leds_off(LEDS_RED);
					}
					break;
				case 6:
					/* gate lock command */
					if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & AUTO_OPENING) == 0)){
						home_status &= ~GATE_UNLOCKED;
						leds_off(LEDS_GREEN);
						leds_on(LEDS_RED);
					}
					break;
				default:
					break;
			}
		} else if(ev == alarm_blink){
			// A message from the alarm_blink process has arrived. We must
			// change the leds in the alarm way.
			// It may happen that gate_node_alarm_led_process has sent the alarm_blink
			// event just before the "deactivate alarm" was issued. This
			// possible race is taken into account.
			if((home_status & ALARM_ACTIVE) != 0){
				if(leds_get() != 0){
					leds_off(LEDS_ALL);
				} else {
					leds_on(LEDS_ALL);
				}
			}
		} else if(ev == opening_blink){
			// A message from the alarm_blink process has arrived. We must
			// change the leds in the auto-opening way.
			// Like in the alarm_blink case, a possible race is taken into account.
			if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & AUTO_OPENING) != 0)){
				if((home_status & GATE_UNLOCKED) == 0){
					// If the gate was locked, we temporarily unlock it.
					// This is done by means of the leds.
					leds_on(LEDS_GREEN);
					leds_off(LEDS_RED);
				}
				// In order to have the blue led turned on the first time,
				// turned off the second time and so on, the process
				// send us the number of occurrence of this message.,
				// and we use the occurrence number in order to understand
				// if we have to turn on or turn off the blue led.
				if(((int)data%2) != 0){
					leds_on(LEDS_BLUE);
				} else {
					leds_off(LEDS_BLUE);
				}
			}
		} else if(ev == opening_blink_stop){
			// A message from the alarm_blink process has arrived, telling us
			// we must go in the state before the auto-opening.
			home_status &= ~AUTO_OPENING;
			if(((home_status & ALARM_ACTIVE) == 0) && ((home_status & GATE_UNLOCKED) == 0)){
				leds_off(LEDS_GREEN);
				leds_on(LEDS_RED);
			}
			strcpy(out_msg, "stop");
			r_send_to_cu(out_msg);

			// There is no need of turning off the blue led, since:
			// if the opening process was not interrupted, the final state is off;
			// if the opening process was interrupted while blue on, blu will be turned off by alarm deactivation;
			// if the opening process was interrupted while blue off, the led is already off
		}
	}
	PROCESS_END();
	return 0;
}

PROCESS_THREAD(gate_node_alarm_blink_process, ev, data)
{
	PROCESS_BEGIN();
	static struct etimer blink_timer;
	etimer_set(&blink_timer,CLOCK_SECOND*2);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&blink_timer)){
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
	static uint8_t blinked;
	static struct etimer blink_timer;
	etimer_set(&blink_timer,CLOCK_SECOND*2);

	for(blinked = 1; blinked < 9; blinked++){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&blink_timer)){
			process_post(&gate_node_main_process, opening_blink, (void*)(int)blinked);
			if(blinked != 8){
				etimer_reset(&blink_timer);
			}
		}
	}
	process_post(&gate_node_main_process, opening_blink_stop, NULL);
	PROCESS_END();
	return 0;
}
