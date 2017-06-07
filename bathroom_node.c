// TODO: fill
/*
 * bathroom.c
 *
 *  Created on: 2017-06-06
 *    Modified:
 *      Author: Alessandro Martinelli
 */

#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "dev/button-sensor.h"
#include "dev/leds.h"
#include "dev/sht11/sht11-sensor.h"
#include "random.h"
//#include <math.h>

#define LOWER_THRESHOLD 			40
#define UPPER_THRESHOLD 			65
#define RANDOM_MAX_VALUE 			5
#define SHOWER_ACTIVE				0x80	/* 1 if alarm is active */
#define VENTILATION_ACTIVE			0x40	/* 1 if automatic opening is occurring */
#define LOWER_TH_EXCEDEED			0x20	/* 1 if the gate is unlocked */
#define UPPER_TH_EXCEDEED			0x10	/* 1 if the gate has communicated the end of the auto-opening procedure */

int sensor_humidity(){
	SENSORS_ACTIVATE(sht11_sensor);
	int relative_humidity = (sht11_sensor.value(SHT11_SENSOR_HUMIDITY)/164);
	SENSORS_DEACTIVATE(sht11_sensor);
	return relative_humidity;
	/*
	 	float adc_value = sht11_sensor.value(SHT11_SENSOR_HUMIDITY)*100;
		float voltage = (adc_value/4095)*5;
		float percentRH = (voltage-0.958)/0.0307;

		printf("%ld.%03u\n", (long)f, (unsigned)((f-floor(f))*1000));

		float raw = sht11_sensor.value(SHT11_SENSOR_HUMIDITY);
		float hum = -4 + (0.0504*raw) + ((-2.8 * powf(10, -6))*(raw*raw));
		printf("%ld\n", (long)hum);
	 */
}

static process_event_t increase_humidity;
static process_event_t decrease_humidity;

/*---------------------------------------------------------------------------*/
PROCESS(bathroom_node_main_process, "Bathroom Node Main Process");
PROCESS(bathroom_node_shower_process, "Bathroom Node Shower Process");
PROCESS(bathroom_node_ventilation_process, "Bathroom Node Ventilation Process");
AUTOSTART_PROCESSES(&bathroom_node_main_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(bathroom_node_main_process, ev, data)
{
	PROCESS_BEGIN();

	increase_humidity = process_alloc_event();
	decrease_humidity = process_alloc_event();
	static int humidity_percentage = 0;
	static uint8_t bathroom_status = 0;
	SENSORS_ACTIVATE(button_sensor);
	// TODO: azzerare tutti i led

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == sensors_event && data == &button_sensor){
			if((bathroom_status & SHOWER_ACTIVE) == 0){
				// The shower has been turned on, thus humidity is being produced
				bathroom_status |= SHOWER_ACTIVE;
				process_start(&bathroom_node_shower_process, NULL);
			} else {
				// The shower has been turned off. Humidity is no more produced
				bathroom_status &= ~SHOWER_ACTIVE;
				process_exit(&bathroom_node_shower_process);
				if((bathroom_status & LOWER_TH_EXCEDEED) == 0){
					humidity_percentage = 0;
				}
			}
		} else if(ev == increase_humidity){
			// A humidity rise. This may happen only if the
			// shower is active; thus, if a spurious event of
			// this kind occurs when the shower is off, it must be ignored.
			if((bathroom_status & SHOWER_ACTIVE) != 0){
				// shower active --> the raise is "valid"
				if(humidity_percentage == 0){
					// Initial value is taken from actual sensor
					humidity_percentage = sensor_humidity();
				}
				humidity_percentage += (uint8_t)data;
				printf("[bathroom node]: humidity has increased, now it is %d\n", humidity_percentage);
				if(humidity_percentage > UPPER_THRESHOLD){
					if((bathroom_status & UPPER_TH_EXCEDEED) == 0){
						// The upper threshold has been excedeed just now.
						// The ventilation system has to be started
						bathroom_status |= UPPER_TH_EXCEDEED;
						bathroom_status |= VENTILATION_ACTIVE;
						process_start(&bathroom_node_ventilation_process, NULL);
						leds_on(LEDS_RED);
						leds_on(LEDS_BLUE);
					}
				} else if (humidity_percentage > LOWER_THRESHOLD){
					if((bathroom_status & LOWER_TH_EXCEDEED) == 0){
						// The upper threshold has been excedeed just now.
						bathroom_status |= LOWER_TH_EXCEDEED;
						leds_on(LEDS_GREEN);
					}
				}
			} else {
				// raise_humidity event occurred when shower already finished:
				// it is a spurious event, and must not be taken into account.
			}
		} else if(ev == decrease_humidity){
			// If a user turn off the shower while the ventilation system is active,
			// the ventilation system has however to complete its work. Thus, it is not
			// important to check if the shower is currently used or not.
			humidity_percentage -= (uint8_t)data;
			printf("[bathroom node]: humidity has decreased, now it is %d\n", humidity_percentage);
			if(humidity_percentage < LOWER_THRESHOLD){
				if((bathroom_status & LOWER_TH_EXCEDEED) != 0){
					// The lower threshold was excedeed until now.
					// The ventilation system can be stopped
					bathroom_status &= ~LOWER_TH_EXCEDEED;
					bathroom_status &= ~VENTILATION_ACTIVE;
					process_exit(&bathroom_node_ventilation_process);
					leds_off(LEDS_GREEN);
					leds_off(LEDS_BLUE);
				}
			} else if(humidity_percentage < UPPER_THRESHOLD){
				if((bathroom_status & UPPER_TH_EXCEDEED) != 0){
					// The upper threshold was excedeed until now.
					bathroom_status &= ~UPPER_TH_EXCEDEED;
					leds_off(LEDS_RED);
				}
			}
		}
	}

	PROCESS_END();
	return 0;
}

PROCESS_THREAD(bathroom_node_shower_process, ev, data)
{
	PROCESS_BEGIN();
	printf("[bathroom node]: shower turned on\n");

	static uint8_t increase;
	static struct etimer timer;
	etimer_set(&timer,CLOCK_SECOND*3);

	while(1){
		PROCESS_WAIT_EVENT();
		if(etimer_expired(&timer)){
			increase = ((random_rand()%(RANDOM_MAX_VALUE+1))+1);
			process_post(&bathroom_node_main_process, increase_humidity, (void*)increase);
			etimer_restart(&timer);
		} else if(ev == PROCESS_EVENT_EXIT){
			printf("[bathroom node]: shower turned off\n");
			// TODO: maybe here I should stop the timer?
		}
	}

	PROCESS_END();
	return 0;
}

PROCESS_THREAD(bathroom_node_ventilation_process, ev, data)
{
	PROCESS_BEGIN();
	printf("[bathroom node]: ventilation system turned on\n");

	static uint8_t decrease;
	static struct etimer timer;
	etimer_set(&timer, CLOCK_SECOND*3);

	while(1){
		PROCESS_WAIT_EVENT();
		if(etimer_expired(&timer)){
			decrease = (RANDOM_MAX_VALUE+2);
			process_post(&bathroom_node_main_process, decrease_humidity, (void*)decrease);
			etimer_restart(&timer);
		} else if(ev == PROCESS_EVENT_EXIT){
			printf("[bathroom node]: ventilation system turned off\n");
			// TODO: maybe here I should stop the timer?
		}
	}

	PROCESS_END();
	return 0;
}
















