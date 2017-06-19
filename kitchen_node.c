/*
 * kitchen_node.c
 *
 *  Created on: 2017-06-06
 *      Author: Alessandro Martinelli
 */

#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "dev/button-sensor.h"
#include "net/rime/rime.h"
#include "dev/leds.h"
#include "dev/sht11/sht11-sensor.h"
#include "random.h"
#include "stdlib.h" /* For strtol */

#define MAX_RETRANSMISSIONS 	5
#define RANDOM_MAX_VALUE 		30
#define CU_NODE_ADDR_0			3
#define CU_NODE_ADDR_1			0

// Event for forwarding a message that has arrived from the central unit
static process_event_t message_from_central_unit;

// Event for signaling the detection of a fire
static process_event_t fire_detected_event;

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	// printf("[kitchen node]: runicast message received from %d.%d: '%s', %d\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr(), *(uint8_t*)packetbuf_dataptr());
	process_post(NULL, message_from_central_unit, packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	// printf("[kitchen node]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
	printf("[kitchen node]: runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static const struct runicast_callbacks runicast_calls = {recv_runicast, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

// The random temperature increase we simulate when the button is clicked
static uint16_t random_increase;

/*
 * This method sample the temperature, and adds to this value
 * the random quantity, possibly set in the main flow.
 * After being considered, the random quantity has to be reset.
 * In order to do this and to avoid critical races,
 * we capture the value of random at the beginning of the method,
 * and then we reset random only if THE CONSIDERED COPY of random
 * is not 0. This is to avoid the following situation: random is 0;
 * we read random as 0; random is then set in the main flow; after
 * this, we check if random is 0 and, since it is not 0, we reset
 * its value, thus losing it.
 */
uint16_t obtain_temperature(){
	SENSORS_ACTIVATE(sht11_sensor);

	uint16_t local_random_increase = random_increase;
	uint16_t temperature = ((sht11_sensor.value(SHT11_SENSOR_TEMP)/10-396)/10);
	SENSORS_DEACTIVATE(sht11_sensor);

	temperature += local_random_increase;
	if(local_random_increase != 0){
		random_increase = 0;
	}

	return temperature;
}

void turn_on_camera(){
	leds_on(LEDS_GREEN);
	leds_off(LEDS_RED);
}

void turn_off_camera(){
	leds_off(LEDS_GREEN);
	leds_on(LEDS_RED);
}

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
PROCESS(kitchen_node_main_process, "Kitchen Node Main Process");
PROCESS(kitchen_node_camera_process, "Kitchen Node Camera Process");
AUTOSTART_PROCESSES(&kitchen_node_main_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(kitchen_node_main_process, ev, data)
{
	PROCESS_EXITHANDLER(runicast_close(&runicast));

	PROCESS_BEGIN();

	static struct etimer sampling_timer;
	char out_msg[10];
	char in_msg[10];
	static uint16_t temperature;
	static uint8_t camera_on;
	static uint16_t warning_threshold;

	camera_on = 0;
	random_increase = 0;
	warning_threshold = 40;

	etimer_set(&sampling_timer, CLOCK_SECOND*10);
	message_from_central_unit = process_alloc_event();
	fire_detected_event = process_alloc_event();

	leds_off(LEDS_GREEN); 	// green led on if camera on
	leds_on(LEDS_RED);		// red led on if camera off
	leds_off(LEDS_BLUE);	// blue led unused
	SENSORS_ACTIVATE(button_sensor);
	runicast_open(&runicast, 144, &runicast_calls);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == sensors_event && data == &button_sensor){
			// If the button is pressed, a new process is started,
			// which has to turn the camera on and return a FIRE message
			// or a FALSE ALARM message. In this simulation, the decision
			// is taken basing of whether the button has been pressed
			// again or not
			if(!camera_on){
				// If the camera is switched on, we don't extract a new random number
				random_increase = (random_rand()%(RANDOM_MAX_VALUE+1));
				printf("[kitchen node]: random extracted: %d\n", random_increase);
			}
		} else if(ev == PROCESS_EVENT_TIMER && etimer_expired(&sampling_timer)){
			// It is time to sample the temperature. If the button has been
			// pressed between the previous measuration and this one,
			// the extracted random value will be added to the actually
			// sampled value.
			temperature = obtain_temperature();
			printf("[kitchen node]: Measured temperature is %d\n", temperature);
			if (temperature > warning_threshold){
				// The threshold has been exceeded, thus the camera has to be
				// switched on, so that it can tell us if a fire occurred.
				camera_on = 1;
				process_start(&kitchen_node_camera_process, NULL);
			}
			etimer_reset(&sampling_timer);
		} else if(ev == fire_detected_event){
			// A fire has been detected: we have to inform the central unit
			// of that event along with the current temperature.
			sprintf(out_msg, "fi%d", temperature);
			r_send_to_cu(out_msg);
		} else if(ev == message_from_central_unit){
			// A message from the central unit has arrived
			strcpy(in_msg, (char*)data);
			if((strncmp(in_msg, "th", 2)) == 0){
				// The central unit has sent the new threshold value
				long threshold = strtol(in_msg+2, NULL, 10);
				if (threshold > 0){
					warning_threshold = (uint16_t)threshold;
					printf("[kitchen node]: alarm_threshold is now %d\n", warning_threshold);
				}
			} else if((strcmp(in_msg, "camoff")) == 0){
				// The central unit has sent the 'turn off the camera' command
				process_exit(&kitchen_node_camera_process);
				// In this case a PROCESS_EVENT_EXITED is not returned, so we have to deactivate
				// the camera manually.
				camera_on = 0;
			}
		} else if(ev == PROCESS_EVENT_EXITED){
			// The camera has not detected anything, thus it has been already
			// turned off and the camera process has terminated.
			// We have to make the 'camera_on' flag consistent with this situation.
			// printf("[kitchen node]: PROCESS_EVENT_EXITED received. In particular, ev %d, data %s\n", ev, data);
			camera_on = 0;
		}
	}
	PROCESS_END();
	return 0;
}


PROCESS_THREAD(kitchen_node_camera_process, ev, data)
{
	PROCESS_BEGIN();

	turn_on_camera();

	/* Fire detection procedure starts here */
	static struct etimer camera_timer;
	etimer_set(&camera_timer, CLOCK_SECOND*4);
	PROCESS_WAIT_EVENT();

	if(ev == sensors_event && data == &button_sensor){
		// The button has been pressed: we simulate the fire detection
		// printf("[kitchen node]: A fire has been detected by the camera\n");
		process_post(&kitchen_node_main_process, fire_detected_event, NULL);

		// We wait to be killed by the main process
		PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_EXIT);
		turn_off_camera();
	} else if(ev == PROCESS_EVENT_TIMER && etimer_expired(&camera_timer)){
		// The timer has expired and no button has been pressed: No fire has been detected
		turn_off_camera();
	}

	PROCESS_END();
	return 0;
}


















