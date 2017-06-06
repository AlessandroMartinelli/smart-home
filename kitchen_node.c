// TODO: fill
/*
 * door_node.c
 *
 *  Created on: 2017-06-06
 *    Modified:
 *      Author: Alessandro Martinelli
 */

/*
 * Here we need 2 process: one periodically sample the temperature,
 * the other wait for button event. When a button is pressed,
 * the camera is activated, and the system wait for some seconds.
 * When the timer has elapsed, the simulated temperature value
 * is the sampled value + rand1 if the button has been pressed 1 time
 * or value + rand1 + rand2 if the button has been pressed 2 times.
 * then if the temperature is below a threshold, a fire is detected
 * and the central unit is contacted.
 * The process waiting for a button will also wait for an input
 * containing the threshold.
 */

/*
 * TODO Tutto sbagliato: in un processo si attende la pressione del bottone e, nel caso,
 * si campiona la temperatura e, se c'e' l'incendio, si manda un messaggio al processo
 * principale. Quest'ultimo e' in carica di ricevere tutti i messaggi, fra cui "allarme scoppiato",
 * "cambia la soglia"
 */

/*
 * No ma guarda, qui in realta' ne basta uno, di processo.
 */

#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "dev/button-sensor.h"
#include "net/rime/rime.h"
// #include "string.h"
#include "dev/leds.h"
#include "dev/sht11/sht11-sensor.h"
#include "random.h"
#include "stdlib.h" /* For strtol */

#define MAX_RETRANSMISSIONS 5
#define RANDOM_MAX_VALUE 30

// Central_unit can send 2 type of messages: change threshold or turn off camera
static process_event_t message_from_central_unit;

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	printf("[kitchen node]: runicast message received from %d.%d: '%s'\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr());
	process_post(NULL, message_from_central_unit, packetbuf_dataptr());
}

static void sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("[kitchen node]: runicast message sent to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static void timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("[kitchen node]: runicast message timed out when sending to %d.%d, retransmissions %d\n", to->u8[0], to->u8[1], retransmissions);
}

static const struct runicast_callbacks runicast_calls = {recv_runicast, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

/*---------------------------------------------------------------------------*/
PROCESS(kitchen_node_main_process, "Kitchen Node Main Process");
//TODO PROCESS(kitchen_node_temperature_process, "Kitchen Node Temperature Process");
AUTOSTART_PROCESSES(&kitchen_node_main_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(kitchen_node_main_process, ev, data)
{
	PROCESS_EXITHANDLER(runicast_close(&runicast));

	PROCESS_BEGIN();
	static struct etimer button_timer;
	char msg[8];
	static uint8_t button_count = 0;
	static uint16_t temperature;
	static uint16_t alarm_threshold = 60;
	leds_off(LEDS_GREEN);
	leds_on(LEDS_RED);
	leds_off(LEDS_BLUE);
	SENSORS_ACTIVATE(button_sensor);
	runicast_open(&runicast, 144, &runicast_calls);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == sensors_event && data == &button_sensor){
			button_count++;
			if(button_count == 1){
				etimer_set(&button_timer, CLOCK_SECOND*4);
				leds_on(LEDS_GREEN);
				leds_off(LEDS_RED);
			}
		} else if(ev == PROCESS_EVENT_TIMER && etimer_expired(&button_timer)){
			SENSORS_ACTIVATE(sht11_sensor);
			temperature = ((sht11_sensor.value(SHT11_SENSOR_TEMP)/10-396)/10);
			SENSORS_DEACTIVATE(sht11_sensor);
			// TODO printf("[kitchen node]: temperature is %d, button_count is %d\n", temperature, button_count);
			if(button_count > 0){
				temperature += (random_rand()%(RANDOM_MAX_VALUE+1));
				// TODO printf("[kitchen node]: temperature is %d\n", temperature);
			} if(button_count > 1) {
				temperature += (random_rand()%(RANDOM_MAX_VALUE+1));
				// TODO printf("[kitchen node]: temperature is %d\n", temperature);
			}
			printf("[kitchen node]: temperature is %d\n", temperature);
			if(temperature > alarm_threshold){
				printf("[kitchen node]: fire detected!\n");
				if(!runicast_is_transmitting(&runicast)) {
					sprintf(msg, "fi%d", temperature);
					linkaddr_t recv;
					packetbuf_copyfrom(&msg, strlen(msg)+1);
					recv.u8[0] = 1;
					recv.u8[1] = 0;
					printf("%u.%u: sending runicast to address %u.%u\n",
							linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], recv.u8[0], recv.u8[1]);
					runicast_send(&runicast, &recv, MAX_RETRANSMISSIONS);
				} else {
					// The previous transmission has not finished yet
					printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
				}
			} else {
				// The camera has not detected a fire: turn off the camera
				leds_off(LEDS_GREEN);
				leds_on(LEDS_RED);
			}
			button_count = 0;
		/* Handling of a message coming from the central unit */
		} else if(ev == message_from_central_unit){
			strcpy(msg, (char*)data);
			/* Handling of 'change threshold' command */
			if((strncmp(msg, "th", 2)) == 0){
				long threshold = strtol(msg+2, NULL, 10);
				// TODO printf("[kitchen node]: received %ld from serial line.\n", threshold);
				if (threshold > 0){
					alarm_threshold = (uint16_t)threshold;
					// TODO printf("[kitchen node]: now alarm_threshold is %d\n", alarm_threshold);
				}
			/* Hadling of the 'turn off the camera' command */
			} else if((strcmp(msg, "camoff")) == 0){
				leds_off(LEDS_GREEN);
				leds_on(LEDS_RED);
			}
		}
	}

	PROCESS_END();
	return 0;
}
































