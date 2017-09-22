/*
 * door_node.c
 *
 *  Created on: 2017-06-05
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
#include "dev/button-sensor.h"
#include "net/rime/rime.h"
#include "string.h"
#include "dev/leds.h"
#include "dev/sht11/sht11-sensor.h"

#define ALARM_ACTIVE		0x80	/* 1 if alarm is active */
#define AUTO_OPENING		0x40	/* 1 if automatic opening is occurring */
#define LIGHTS_ON			0x20	/* 1 if garden external light are on */
#define MAX_RETRANSMISSIONS 5
#define CU_NODE_ADDR_0			3
#define CU_NODE_ADDR_1			0

/*---Queue Structure---------------------------------------------------------*/
#define QUEUE_ELEMENTS 			5

// The actual circular queue
int queue[QUEUE_ELEMENTS];

// Used to store the position that will be used for inserting the next element
uint8_t queue_insert_index;

/*
 * This method initializes the queue. The first element will be put in position 0.
 */
void queue_init(){
	queue_insert_index = 0;
	uint8_t i;
	for(i = 0; i < QUEUE_ELEMENTS; i++){
		queue[i] = 0;
	}
}

/*
 * Insert the given element in the 'queue_insert_index' position
 * and set the index of the next value to be inserted.
 * We use a circular queue, thus after using the last position
 * of the array we will override the first position.
 */
void queue_insert(int new){
	queue[queue_insert_index] = new;
	queue_insert_index = (queue_insert_index + 1) % QUEUE_ELEMENTS;
}

/*
 * This method simply computes the mean value of the temperature
 * values stored in the queue.
 */
int queue_mean_get(){
	int sum = 0;
	// Queue of max 256 elements, here we need only 5
	uint8_t i;
	for(i = 0; i<QUEUE_ELEMENTS; i++){
		sum += queue[i];
	}
	return (sum/QUEUE_ELEMENTS);
}
/*---------------------------------------------------------------------------*/

static process_event_t message_from_central_unit;
static process_event_t alarm_blink;
static process_event_t opening_blink;
static process_event_t opening_blink_stop;

static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from){
	// printf("[door node]: broadcast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());

	// Since the processes have not been declared yet, the message is sent to all processes
	process_post(NULL, message_from_central_unit, (char *)packetbuf_dataptr());
}

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	// printf("[door node]: runicast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());

	// Since the processes have not been declared yet, the message is sent to all processes
	process_post(NULL, message_from_central_unit, (char *)packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	// printf("[door node]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	printf("[door node]: runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
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

/*---------------------------------------------------------------------------*/
PROCESS(door_node_main_process, "Door Node Main Process");
PROCESS(door_node_alarm_blink_process, "Door Node Alarm Led Process");
PROCESS(door_node_opening_blink_process, "Door Node Opening Led Process");
PROCESS(door_node_temperature_process, "Door Node Temperature Process");
AUTOSTART_PROCESSES(&door_node_main_process, &door_node_temperature_process);
/*---------------------------------------------------------------------------*/

PROCESS_THREAD(door_node_main_process, ev, data)
{
	PROCESS_EXITHANDLER(broadcast_close(&broadcast));
	PROCESS_EXITHANDLER(runicast_close(&runicast));

	PROCESS_BEGIN();

	// TODO: comment
	uint8_t command;		// used to store the command sent by the central unit
	char out_msg[10];		// stores the message to be sent to the central unit

	home_status = 0;		// initializes the system status

	// This customized message is declared here even if it is used in the
	// broadcast received function. But it's ok, since the broadcast_open function
	// is called only after the custom event initialization.
	message_from_central_unit = process_alloc_event();
	alarm_blink = process_alloc_event();
	opening_blink = process_alloc_event();
	opening_blink_stop = process_alloc_event();
	broadcast_open(&broadcast, 129, &broadcast_call);
	runicast_open(&runicast, 144, &runicast_calls);

	// initialize the circular queue in charge of storing temperature values
	queue_init();

	SENSORS_ACTIVATE(button_sensor);

	// At the beginning, garden lights are turned off.
	leds_off(LEDS_GREEN);
	leds_on(LEDS_RED);
	leds_off(LEDS_BLUE);

	while(1){
		// We wait for either a message from the central unit,
		// a button click or for a message from another process.
		PROCESS_WAIT_EVENT();
		if(ev == message_from_central_unit){
			// Message from the central unit
			command = *(uint8_t*)data;
			switch(command){
				case 1:
					/* alarm activation command */
					if((home_status & ALARM_ACTIVE) == 0){
						home_status |= ALARM_ACTIVE;
						process_start(&door_node_alarm_blink_process, NULL);
					}
					break;
				case 2:
					/* alarm deactivation command */
					if((home_status & ALARM_ACTIVE) != 0){
						home_status &= ~(ALARM_ACTIVE);
						process_exit(&door_node_alarm_blink_process);
						// leds has to return in their previous state
						if((home_status & LIGHTS_ON) != 0){
							leds_on(LEDS_GREEN);
							leds_off(LEDS_RED);
							leds_off(LEDS_BLUE);
						} else {
							leds_off(LEDS_GREEN);
							leds_on(LEDS_RED);
							leds_off(LEDS_BLUE);
						}
					}
					break;
				case 3:
					/* auto opening command */
					if(((home_status & ALARM_ACTIVE) == 0) || ((home_status & AUTO_OPENING) == 0)){
						home_status |= AUTO_OPENING;
						process_start(&door_node_opening_blink_process, NULL);
					}
					break;
				case 4:
					/* temperature mean value command */
					if((home_status & ALARM_ACTIVE) == 0){
						sprintf(out_msg, "tem%d", queue_mean_get());
						r_send_to_cu(out_msg);
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
			home_status &= ~AUTO_OPENING;
			leds_off(LEDS_BLUE);
			strcpy(out_msg, "stop");
			r_send_to_cu(out_msg);

			// There is no need of turning off the blue led, since:
			// if the opening process was not interrupted, the final state is off;
			// if the opening process was interrupted while blue on, blu will be turned off by alarm deactivation;
			// if the opening process was interrupted while blue off, the led is already off
		} else if(ev == sensors_event && data == &button_sensor){
			if((home_status & ALARM_ACTIVE) == 0){
				// If alarm is not active, toggle garden lights
				if((home_status & LIGHTS_ON) == 0){
					home_status |= LIGHTS_ON;
					leds_on(LEDS_GREEN);
					leds_off(LEDS_RED);
				} else {
					home_status &= ~LIGHTS_ON;
					leds_off(LEDS_GREEN);
					leds_on(LEDS_RED);
				}
			}
		}
	}
	PROCESS_END();
	return 0;
}

/*
 * This process, started only when the alarm is activated,
 * is in charge of sending periodic events to the main
 * process so that the latter one knows it has to blink all leds.
 */
PROCESS_THREAD(door_node_alarm_blink_process, ev, data)
{
	PROCESS_BEGIN();
	static struct etimer blink_timer;
	etimer_set(&blink_timer,CLOCK_SECOND*2);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&blink_timer)){
			process_post(&door_node_main_process, alarm_blink, NULL);
			etimer_reset(&blink_timer);
		}
	}
	PROCESS_END();
	return 0;
}

/*
 * This process, started only when the automatic opening
 * and closing of the door is issued, is in charge of
 * waiting for the correct amount of time to elapse, and
 * then communicating the main process to blink.
 * When the blinking should be stopped, this process
 * sends another events to the main process.
 */
PROCESS_THREAD(door_node_opening_blink_process, ev, data)
{
	PROCESS_BEGIN();
	static uint8_t blinked;
	static struct etimer blink_timer;
	etimer_set(&blink_timer,CLOCK_SECOND*2);

	for(blinked = 1; blinked < 16; blinked++){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&blink_timer)){
			if(blinked != 15){
				etimer_reset(&blink_timer);
			}
			if(blinked >= 7){
				process_post(&door_node_main_process, opening_blink, (void*)(int)blinked);
			}
		}
	}
	process_post(&door_node_main_process, opening_blink_stop, NULL);
	PROCESS_END();
	return 0;
}

/*
 * This process is in charge of sampling the temperature every 10 seconds;
 * Each sample is stored in a circular buffer (implementing a FIFO queue),
 * where only 5 elements can be stored. So, every time a new element is put
 * into the queue, it replaces the oldest one.
 */
PROCESS_THREAD(door_node_temperature_process, ev, data)
{
	PROCESS_BEGIN();
	static struct etimer temperature_timer;
	etimer_set(&temperature_timer, CLOCK_SECOND*10);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&temperature_timer)){
			SENSORS_ACTIVATE(sht11_sensor);
			queue_insert(((sht11_sensor.value(SHT11_SENSOR_TEMP)/10-396)/10));
			SENSORS_DEACTIVATE(sht11_sensor);
			etimer_reset(&temperature_timer);
		}
	}
	PROCESS_END();
	return 0;
}
