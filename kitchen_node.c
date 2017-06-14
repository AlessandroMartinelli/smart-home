// TODO: fill
/*
 * kitchen_node.c
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
#include <stdbool.h> /* For boolean */

#define MAX_RETRANSMISSIONS 5
#define RANDOM_MAX_VALUE 30

// Central_unit can send 2 type of messages: change threshold or turn off camera
static process_event_t message_from_central_unit;
static process_event_t fire_detected_event;

static void recv_runicast(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
	// TODO tutte queste stampe io le toglierei, o perlomeno le lascerei commentate
	printf("[kitchen node]: runicast message received from %d.%d: '%s', %d\n", from->u8[0], from->u8[1], (char *)packetbuf_dataptr(), *(uint8_t*)packetbuf_dataptr());
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

static uint16_t random_jump;

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
static uint16_t obtain_temperature(){
	SENSORS_ACTIVATE(sht11_sensor);

	uint16_t local_random = random_jump;
	uint16_t temperature = ((sht11_sensor.value(SHT11_SENSOR_TEMP)/10-396)/10);
	SENSORS_DEACTIVATE(sht11_sensor);

	temperature += local_random;
	if(local_random != 0){
		random_jump = 0;
	}

	return temperature;
}

/*
 * static uint8_t detect_fire(){
 * }
 */

static void turn_on_camera(){
	leds_on(LEDS_GREEN);
	leds_off(LEDS_RED);
}

static void turn_off_camera(){
	leds_off(LEDS_GREEN);
	leds_on(LEDS_RED);
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
	char msg[8];
	static uint16_t temperature;
	//static uint8_t button_count;
	// static uint16_t temperature;
	//button_count = 0;
	random_jump = 0;
	static uint16_t warning_threshold = 40;

	etimer_set(&sampling_timer, CLOCK_SECOND*10);
	message_from_central_unit = process_alloc_event();
	fire_detected_event = process_alloc_event();

	leds_off(LEDS_GREEN); 	// on if camera on
	leds_on(LEDS_RED);		// on if camera off
	leds_off(LEDS_BLUE);	// unused
	SENSORS_ACTIVATE(button_sensor);
	runicast_open(&runicast, 144, &runicast_calls);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == sensors_event && data == &button_sensor){
			// If the button is pressed, a new process is started,
			// which has to turn the camera on and return a FIRE
			// or FALSE ALARM message. In this simulation, the decision
			// is taken basing of whether the button has been pressed
			// again or not (or by means of a random variable)
			random_jump = (random_rand()%(RANDOM_MAX_VALUE+1));
			printf("[kitchen node]: random extracted. It is %d\n", random_jump);
		} else if(ev == PROCESS_EVENT_TIMER && etimer_expired(&sampling_timer)){
			temperature = obtain_temperature();
			printf("[kitchen node]: periodic sampling. Temperature is %d\n", temperature);
			if (temperature > warning_threshold){
				// TODO start the camera process
				process_start(&kitchen_node_camera_process, NULL);
			}
			etimer_reset(&sampling_timer);
		} else if(ev == fire_detected_event){
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
				// TODO: questo messaggio e' critico: va consegnato a tutti i costi. Basterebbe fare un while
				printf("[Serial Port Output]: It was not possible to issue the command. Try again later\n");
			}
		/* Handling of a message coming from the central unit */
		} else if(ev == message_from_central_unit){
			strcpy(msg, (char*)data);
			//printf("[kitchen node]: %d, %d\n", *(int*)data, (int*)data);
			//uint8_t tmp = *(int*)data;
			/* Handling of 'change threshold' command */
			//printf("msg[0] is %c\n", msg[0]);

			if((strncmp(msg, "th", 2)) == 0){
				long threshold = strtol(msg+2, NULL, 10);
				// TODO printf("[kitchen node]: received %ld from serial line.\n", threshold);
				if (threshold > 0){
					warning_threshold = (uint16_t)threshold;
					// TODO printf("[kitchen node]: now alarm_threshold is %d\n", alarm_threshold);
				}
			/* Hadling of the 'turn off the camera' command */
			} else if((strcmp(msg, "camoff")) == 0){
				process_exit(&kitchen_node_camera_process);
			}
		}
	}

	PROCESS_END();
	return 0;
}


PROCESS_THREAD(kitchen_node_camera_process, ev, data)
{
	PROCESS_BEGIN();

	printf("[kitchen node]: Hello from camera process\n");

	// accensione fotocamera
	turn_on_camera();

	/* Fire detection procedure starts here */
	static struct etimer camera_timer;
	etimer_set(&camera_timer, CLOCK_SECOND*4);
	PROCESS_WAIT_EVENT();

	if(ev == sensors_event && data == &button_sensor){
		// The button has been pressed: we simulate the fire detection
		printf("[kitchen node]: A fire has been detected by the camera\n");
		process_post(&kitchen_node_main_process, fire_detected_event, NULL);
		PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_EXIT);
		turn_off_camera();
		printf("[kitchen node]: Goodbye from camera process (killed)\n");
	} else if(ev == PROCESS_EVENT_TIMER && etimer_expired(&camera_timer)){
		// The timer has expired: no button has been pressed. No fire has been detected
		printf("[kitchen node]: No fire has been detected by the camera\n");
		turn_off_camera();
	}

	// TODO se nessun allarme e' stato rilevato mi spengo da solo, altrimenti mi metto in attesa
	// del messaggio di spegnimento (che poi sarebbe il messaggio di morte, in seguito al quale
	// faccio la pulizia, che consiste nello spagnere la fotocamera

	printf("[kitchen node]: Goodbye from camera process (natural death)\n");
	PROCESS_END();
	return 0;
}


















