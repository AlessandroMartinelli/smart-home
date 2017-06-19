/*
 * bathroom_node.c
 *
 *  Created on: 2017-06-06
 *      Author: Alessandro Martinelli
 */

#include "contiki.h"
#include "sys/etimer.h"
#include "stdio.h" /* For printf() */
#include "dev/button-sensor.h"
#include "dev/leds.h"
#include "dev/sht11/sht11-sensor.h"
#include "random.h"

#define LOWER_THRESHOLD 			35
#define UPPER_THRESHOLD 			60
#define RANDOM_MAX_VALUE 			5
#define SHOWER_ACTIVE				0x80	/* 1 if alarm is active */
#define VENTILATION_ACTIVE			0x40	/* 1 if automatic opening is occurring */
#define LOWER_TH_EXCEDEED			0x20	/* 1 if the gate is unlocked */
#define UPPER_TH_EXCEDEED			0x10	/* 1 if the gate has communicated the end of the auto-opening procedure */

int obtain_humidity(){
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

/*
 * This event is sent, along with the temperature
 * increase, when the humidity increases.
 */
static process_event_t increase_humidity;

/*
 * This event is sent, along with the temperature
 * decrease, when the humidity decreases.
 */
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

	// Store the humidity current value. When 0 it is not meaningful.
	static int humidity_percentage;

	/*
	 * Variable used as an array of flags. Those flags store the state
	 * of the system. They are used to perform operations in a consistent manner.
	 */
	static uint8_t bathroom_status;

	increase_humidity = process_alloc_event();
	decrease_humidity = process_alloc_event();

	// initial value is not meaningful
	humidity_percentage = 0;

	// at the beginning shower and ventilation are turned off, and no threshold has been exceeded
	bathroom_status = 0;

	SENSORS_ACTIVATE(button_sensor);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == sensors_event && data == &button_sensor){
			// Button has been clicked. The shower has been either opened or closed.
			if((bathroom_status & SHOWER_ACTIVE) == 0){
				// The shower is being turned on, thus humidity is being produced
				bathroom_status |= SHOWER_ACTIVE;
				process_start(&bathroom_node_shower_process, NULL);
			} else {
				// The shower is being turned off. Humidity is no more produced.
				// Please note the ventilation system is turned off when the humidity
				// has fallen below a certain threshold, not when the shower is turned off.
				bathroom_status &= ~SHOWER_ACTIVE;
				process_exit(&bathroom_node_shower_process);
				if((bathroom_status & LOWER_TH_EXCEDEED) == 0){
					// If the humidity is very low, we consider this value
					// no more meaningful, and the next time the shower will be turned on,
					// presumably not before some hours, we will sample this value again.
					humidity_percentage = 0;
				}
			}
		} else if(ev == increase_humidity){
			// A humidity increase. This may happen only if the
			// shower is active; thus, if a spurious event of
			// this kind occurs when the shower is off, it must be ignored.
			if((bathroom_status & SHOWER_ACTIVE) != 0){
				// shower active --> the raise is "valid"
				if(humidity_percentage == 0){
					// Initial value is taken from actual sensor
					humidity_percentage = obtain_humidity();
				}
				humidity_percentage += (uint8_t)(int)data;
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
			if((bathroom_status & VENTILATION_ACTIVE) != 0){
				// A humidity decrease. This may happen only if the
				// ventilation system is active; thus, if a spurious event of
				// this kind occurs when the ventilation is off, it must be ignored.
				humidity_percentage -= (uint8_t)(int)data;
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
			} else {
				// decrease_humidity event occurred when ventilation already stopped:
				// it is a spurious event, and must not be taken into account.
			}
		}
	}
	PROCESS_END();
	return 0;
}

PROCESS_THREAD(bathroom_node_shower_process, ev, data)
{
	PROCESS_BEGIN();
	// printf("[bathroom node]: shower turned on\n");

	static uint8_t increase;
	static struct etimer timer;
	etimer_set(&timer,CLOCK_SECOND*3);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&timer)){
			// incrase has to be at least 1 (to avoid too much 0 value to occurring)
			increase = ((random_rand()%(RANDOM_MAX_VALUE+1))+1);
			process_post(&bathroom_node_main_process, increase_humidity, (void*)(int)increase);
			etimer_restart(&timer);
		} else if(ev == PROCESS_EVENT_EXIT){
			// printf("[bathroom node]: shower turned off\n");
			etimer_stop(&timer);
		}
	}

	PROCESS_END();
	return 0;
}

PROCESS_THREAD(bathroom_node_ventilation_process, ev, data)
{
	PROCESS_BEGIN();
	// printf("[bathroom node]: ventilation system turned on\n");

	static uint8_t decrease;
	static struct etimer timer;
	etimer_set(&timer, CLOCK_SECOND*3);

	while(1){
		PROCESS_WAIT_EVENT();
		if(ev == PROCESS_EVENT_TIMER && etimer_expired(&timer)){
			decrease = (RANDOM_MAX_VALUE+2);
			process_post(&bathroom_node_main_process, decrease_humidity, (void*)(int)decrease);
			etimer_restart(&timer);
		} else if(ev == PROCESS_EVENT_EXIT){
			// printf("[bathroom node]: ventilation system turned off\n");
			etimer_stop(&timer);
		}
	}

	PROCESS_END();
	return 0;
}
















