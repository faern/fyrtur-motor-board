/*
 * motor.c
 *
 *  Created on: Aug 29, 2020
 *      Author: Marko Juhanne
 */
#include "main.h"
#include "motor.h"
#include "eeprom.h"

motor_status status;
motor_direction direction;

/* LOCATION is the spatial position of the curtain measured in motor revolutions. Due to additional gear mechanism, it takes
 * GEAR_RATIO revolutions in order to actually reach 1 full revolution of the curtain rod. Motor revolution is detected by HALL sensor,
 * which generates 4 interrupts (ticks) per motor revolution.
 *
 * POSITION itself is a measure of curtain position reported by float between 0.0 (fully closed) and 100.0 (fully open) and can be
 * calculated from LOCATION with location_to_position100 (and vice versa with position100_to_location).
 *
 * Maximum POSITION is affected by user-customizable max curtain length (configured via CMD_SET_MAX_CURTAIN_LENGTH). In addition to this,
 * there is the "absolute" limit of full (factory defined) curtain length. However these limits can be ignored using CMD_OVERRIDE_XXX commands and also be
 * re-configured with CMD_SET_MAX_CURTAIN_LENGTH / CMD_SET_FULL_CURTAIN_LENGTH commands.
 */
#define DEG_TO_LOCATION(x) (GEAR_RATIO * 4 * x / 360)
int32_t target_location = 0;
int32_t location = 0;

uint32_t full_curtain_length = DEFAULT_FULL_CURTAIN_LEN;
uint32_t max_curtain_length;

uint16_t minimum_voltage;	// value is minimum voltage * 16 (float stored as interger value)

/* the motor driver gate PWM duty cycle is initially 60/255 when first energized and then adjusted according to target_speed */
#define INITIAL_PWM 60

uint8_t default_speed;
uint8_t target_speed = 0; // target RPM
uint8_t curr_pwm = 0;  // PWM setting

/*
 * When doing calibration the curtain rod is rotated upwards to highest position until motor stalls. The next phase is the endpoint calibration, when the motor is de-energized,
 * which causes the rod to rotate slightly downwards due to curtain tension. We must wait a bit before tension is released and curtain rod
 * is settled in order to correct for the downwards movement. After this time period the motor is considered to be in top position (location = 0)
 */
uint32_t endpoint_calibration_started_timestamp = 0;

/*
 * When calibrating we allow unrestricted movement until calibration procedure is done
 */
uint8_t calibrating = 0;

uint8_t auto_calibration; // If enabled, auto-calibration will roll up the blinds during power up in order to calibrate top curtain position. Enabled by default

/*
 * count how many milliseconds since previous HALL sensor #1 interrupt occurred
 * in order to calculate hall_sensor_interval (and RPM) and also to detect motor stalling
 */
uint32_t hall_sensor_1_idle_time = 0;

uint32_t hall_sensor_1_ticks = 0;	// how many hall sensor #1 ticks(signals) after movement
uint32_t hall_sensor_2_ticks = 0;	// how many hall sensor #2 ticks(signals) after movement

uint32_t hall_sensor_1_interval = 0; // how many milliseconds between Hall sensor #1 ticks

/*
 * Used for stall detection grace period
 * Motor is given some time to gather speed by increasing PWM duty cycle before applying stall detection
 */
uint32_t movement_started_timestamp = 0;

int rotor_position = -1;

uint8_t min_slowdown_speed = DEFAULT_MINIMUM_SLOWDOWN_SPEED;
uint8_t	slowdown_factor = DEFAULT_SLOWDOWN_FACTOR;


motor_command command; // for deferring execution to main loop since we don't want to invoke HAL_Delay in UARTinterrupt handler

// for debugging
int dir_error = 0;
int sensor_ticks_while_stopped = 0;
int sensor_ticks_while_calibrating_endpoint = 0;
uint32_t saved_hall_sensor_1_ticks = 0;	// how many hall sensor #1 ticks(signals) after movement
uint32_t saved_hall_sensor_2_ticks = 0;	// how many hall sensor #2 ticks(signals) after movement


// ----- Commands supported also by original Fyrtur module -----

// commands with 1 parameter
#define CMD_GO_TO	0xdd

// commands without parameter
#define CMD_UP 		0x0add
#define CMD_DOWN 	0x0aee
#define CMD_UP_17 	0x0a0d
#define CMD_DOWN_17	0x0a0e
#define CMD_STOP	0x0acc

#define CMD_OVERRIDE_UP_90		0xfad1
#define CMD_OVERRIDE_DOWN_90	0xfad2
#define CMD_OVERRIDE_UP_6		0xfad3
#define CMD_OVERRIDE_DOWN_6		0xfad4
#define CMD_SET_MAX_CURTAIN_LENGTH	0xfaee	// will be stored to flash memory
#define CMD_SET_FULL_CURTAIN_LENGTH	0xfacc	// will be stored to flash memory
#define CMD_RESET_CURTAIN_LENGTH	0xfa00	// reset maximum curtain length to factory setting (full curtain length). New value is stored to flash memory

#define CMD_GET_STATUS 	0xcccc
#define CMD_GET_STATUS2 0xcccd
#define CMD_GET_STATUS3 0xccce
#define CMD_GET_STATUS4 0xccdd

// ------ Commands supported only by our custom firmware -------

// commands with 1 parameter
#define CMD_EXT_GO_TO				0x10	// target position is the lower 4 bits of the 1st byte + 2nd byte (12 bits of granularity), where lower 4 bits is the decimal part
#define CMD_EXT_SET_SPEED 			0x20	// setting speed via this command will not alter non-volatile memory (so it's safe for limited write-cycle flash memory)
#define CMD_EXT_SET_DEFAULT_SPEED 	0x30	// default speed will be stored to flash memory
#define CMD_EXT_SET_MINIMUM_VOLTAGE	0x40	// minimum voltage. Will be stored to flash memory
#define CMD_EXT_SET_LOCATION		0x50	// location is the lower 4 bits of the 1st byte + 2nd byte (1 sign bit + 11 bits of integer part)
#define CMD_EXT_SET_AUTO_CAL		0x60	// If enabled, auto-calibration will roll up the blinds during power up in order to calibrate top curtain position. Enabled by default
#define CMD_EXT_GO_TO_LOCATION		0x70	// Go to target location (measured in Hall sensor ticks)
#define CMD_EXT_SET_SLOWDOWN_FACTOR 0x80	// Set slowdown factor
#define CMD_EXT_SET_MIN_SLOWDOWN_SPEED 0x90	// Set minimum approach speed

// commands without parameter
#define CMD_EXT_OVERRIDE_DOWN		0xfada	// Continous move down ignoring the max/full curtain length. Maximum movement of 5 revolutions per command
#define CMD_EXT_GET_LOCATION 		0xccd0
#define CMD_EXT_GET_VERSION 		0xccdc
#define CMD_EXT_GET_STATUS 			0xccde
#define CMD_EXT_GET_LIMITS 			0xccdf
#define CMD_EXT_DEBUG	 			0xccd1
#define CMD_EXT_SENSOR_DEBUG 		0xccd2

/****************** EEPROM variables ********************/

typedef enum eeprom_var_t {
	MAX_CURTAIN_LEN_EEPROM = 0,
	FULL_CURTAIN_LEN_EEPROM = 1,
	MINIMUM_VOLTAGE_EEPROM = 2,
	DEFAULT_SPEED_EEPROM = 3,
	AUTO_CAL_EEPROM = 4
} eeprom_var_t;

/* Virtual address defined by the user: 0xFFFF value is prohibited */
uint16_t VirtAddVarTab[NB_OF_VAR] = {0x5555, 0x6666, 0x7777, 0x8888, 0x9999};


void motor_set_default_settings() {
	max_curtain_length = DEFAULT_FULL_CURTAIN_LEN; // by default, max_curtain_length is full_curtain_length
	full_curtain_length = DEFAULT_FULL_CURTAIN_LEN;
	minimum_voltage = DEFAULT_MINIMUM_VOLTAGE;
	default_speed = DEFAULT_TARGET_SPEED;
	auto_calibration = DEFAULT_AUTO_CAL_SETTING;
}

#ifndef SLIM_BINARY
void motor_load_settings() {
	uint16_t tmp;
	if (EE_ReadVariable(VirtAddVarTab[MAX_CURTAIN_LEN_EEPROM], &tmp) != 0) {
		tmp = max_curtain_length = DEFAULT_FULL_CURTAIN_LEN;	// by default, max_curtain_length is full_curtain_length
		EE_WriteVariable(VirtAddVarTab[FULL_CURTAIN_LEN_EEPROM], tmp);
	} else {
		max_curtain_length = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[FULL_CURTAIN_LEN_EEPROM], &tmp) != 0) {
		tmp = full_curtain_length = DEFAULT_FULL_CURTAIN_LEN;
		EE_WriteVariable(VirtAddVarTab[FULL_CURTAIN_LEN_EEPROM], tmp);
	} else {
		full_curtain_length = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[MINIMUM_VOLTAGE_EEPROM], &tmp) != 0) {
		minimum_voltage = DEFAULT_MINIMUM_VOLTAGE;
		tmp = minimum_voltage;
		EE_WriteVariable(VirtAddVarTab[MINIMUM_VOLTAGE_EEPROM], tmp);
	} else {
		minimum_voltage = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[DEFAULT_SPEED_EEPROM], &tmp) != 0) {
		tmp = default_speed = DEFAULT_TARGET_SPEED;
		EE_WriteVariable(VirtAddVarTab[DEFAULT_SPEED_EEPROM], tmp);
	} else {
		default_speed = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[AUTO_CAL_EEPROM], &tmp) != 0) {
		auto_calibration = DEFAULT_AUTO_CAL_SETTING;
		tmp = auto_calibration;
		EE_WriteVariable(VirtAddVarTab[AUTO_CAL_EEPROM], tmp);
	} else {
		auto_calibration = tmp;
	}
}
#endif

void motor_write_setting( eeprom_var_t var, uint16_t value ) {
#ifndef SLIM_BINARY
	uint16_t tmp;
	if (status == Stopped) {
		// motor has to be stopped to change non-volatile settings (writing to FLASH should occur uninterrupted)
		EE_ReadVariable(VirtAddVarTab[var], &tmp);
		if (tmp != value) {
			EE_WriteVariable(VirtAddVarTab[var], value);
		}
	}
#endif
}

uint32_t position100_to_location( float position ) {
	if (position > 100)
		return max_curtain_length;
	return position*max_curtain_length/100;
}


float location_to_position100() {
	if (calibrating) {
		// When calibrating we ignore our position and return 50% instead
		return 50;
	}
	if (location < 0) {	// don't reveal positions higher than top position (should not happen if calibrated correctly)
		return 0;
	}
	if (location > max_curtain_length) {
		return 100;
	}
	return 100*(float)location / max_curtain_length;
}


int get_rpm() {
	if (hall_sensor_1_interval) {
		// 60000 ms in minute
		// 2 hall sensor #1 interrupts per motor revolution
		// GEAR_RATIO motor revolutions per curtain rod revolution
		return 60*1000/GEAR_RATIO/hall_sensor_1_interval/2;
	}
	return 0;
}


/*
 * This function adjusts location when the curtain rod is rotated by motor AS WELL AS by passive movement.
 * Movement is ignored only during calibrating since we are rolling upwards against hard-stop (and to location 0) anyway.
 */
int process_location(motor_direction sensor_direction) {
	if (!calibrating) {
		if (sensor_direction == Up) {
			location--;
			if (direction == Up) {
				if (target_location != -1) {	// if target is -1, force movement up until the motor stalls which causes calibration
					if (location - 1 <= target_location) {	// stop just before the target
						motor_stop();
						return 1;
					}
				}
			}
		} else if (sensor_direction == Down) {
			location++;
			if (direction == Down) {
				if(location + 1 >= target_location) { // stop just before the target
					motor_stop();
					return 1;
				}
			}
		}

		// If motor is rotating, slow it down when approaching the target location
		if (direction != None) {
			int distance_to_target = abs(target_location - location);
			if (distance_to_target < target_speed * slowdown_factor /8) {
				status = Stopping;
				int new_speed = distance_to_target*8/slowdown_factor;
				if (new_speed < min_slowdown_speed)
					new_speed = min_slowdown_speed; // minimum approach speed
				if (new_speed < target_speed)
					target_speed = new_speed;
			}
		}
	}
	return 0;
}


/*
 * Hall sensors will create following interrupts:
 * Upwards movement: HALL1 HIGH, HALL2 HIGH, HALL1 LOW, HALL2 LOW
 * Downwards movement: HALL2 HIGH, HALL1 HIGH, HALL2 LOW, HALL1 LOW
 */
void hall_sensor_callback( uint8_t sensor, uint8_t value ) {

	// This calculation will give following values for rotor_position:
	// Upwards movement: ..., 0, 1, 2, 3, 0, 1, 2, 3, 0, ...
	// Downwards movement: ..., 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, ...
	// Note that changing direction will "skip" 1 position:
	// 	e.g. HALL2_HIGH -> HALL1_LOW -> stop and change direction -> HALL1_HIGH -> HALL2_LOW will translate to:
	//	1, 2, (stop & change dir), 0, 3, ...
	int new_rotor_position = sensor + (1-value)*2;

	if (sensor == HALL_1_SENSOR) {
		hall_sensor_1_ticks++;
		if (hall_sensor_1_ticks > 1) {
			// At least two sensor ticks are needed to calculate interval correctly
			hall_sensor_1_interval = hall_sensor_1_idle_time;	// update time passed between hall sensor interrupts
		}
		hall_sensor_1_idle_time = 0;
	} else {
		hall_sensor_2_ticks++;
	}

	// save for debugging
	if (status == Stopped) {
		sensor_ticks_while_stopped++;
	} else if (status == CalibratingEndPoint) {
		sensor_ticks_while_calibrating_endpoint++;
	}


	if (rotor_position != -1) {
		int diff = (4 + new_rotor_position - rotor_position) & 0x3;
		if (diff == 1) {
			// Sensor direction is UP

			if (direction != Down) {
				// Process Up movement while motor is rotating upwards or disengaged
				process_location(Up);
			} else if (direction == Down ) {
				// Mismatched direction between sensor and motor.
				dir_error++;
			}
		} else if (diff == 3) {
			// Sensor direction is DOWN

			if (direction != Up) {
				// Process Down movement while motor is rotating downwards or disengaged
				process_location(Down);
			} else if (direction == Up) {
				// Mismatched direction between sensor and motor.
				dir_error++;
			}
		} else {
			// Change of direction
			//process_location(direction);
		}
	}

	rotor_position = new_rotor_position;
}


/* Called every 10ms by TIM3 */
void motor_adjust_rpm() {
	if ((status == Moving) || (status == Stopping)) {
		uint32_t speed = get_rpm();
		if (speed < target_speed) {
			if (curr_pwm < 254) {
				curr_pwm++;
				if (target_speed - speed > 2)	// additional acceleration if speed difference is greater
					curr_pwm++;
				if (direction == Up)
					TIM1->CCR4 = curr_pwm;
				else
					TIM1->CCR1 = curr_pwm;
			}
		}

		if (speed > target_speed) {
			if (curr_pwm > 1) {
				curr_pwm--;
				if (speed - target_speed > 2)	// additional acceleration if speed difference is greater
					curr_pwm--;
				if (speed - target_speed > 4)	// additional acceleration if speed difference is greater
					curr_pwm--;
				if (direction == Up)
					TIM1->CCR4 = curr_pwm;
				else
					TIM1->CCR1 = curr_pwm;
			}
		}
	}
}


/*
 * This is periodically (every 1 millisecond) called by SysTick_Handler
 */
void motor_stall_check() {
	if ( (status == Moving) || (status == Stopping) ) {
		// Count how many milliseconds since previous HALL sensor interrupt
		// in order to calculate RPM and detect motor stalling
		hall_sensor_1_idle_time ++;
		if (HAL_GetTick() - movement_started_timestamp > HALL_SENSOR_GRACE_PERIOD) {
			// enough time has passed since motor is energized -> apply stall detection

			if (hall_sensor_1_idle_time > HALL_SENSOR_TIMEOUT) {
				// motor has stalled/stopped

				if ( (status == Stopping) && (hall_sensor_1_idle_time < HALL_SENSOR_TIMEOUT_WHILE_STOPPING) ) {
					// when slowing down, allow longer time to recover from premature stalling
				} else {
					motor_stopped();
					hall_sensor_1_idle_time = 0;
				}
			}
		}
	} else if (status == CalibratingEndPoint) {
		if (HAL_GetTick() - endpoint_calibration_started_timestamp > ENDPOINT_CALIBRATION_PERIOD) {
			// Calibration is done and we are at top position
			status = Stopped;
			calibrating = 0;	// Limits will be enforced from now on
			location = 0;
		}
	}
}

void motor_stopped() {
	if (status != Stopped) {
		// motor has stalled!

		motor_status current_status = status;
		motor_direction current_direction = direction;

		// De-energize the motor
		motor_stop();

		if (current_status == Moving)  {
			if (current_direction == Up) {
				// If motor has stalled abruptly, we assume that we have reached the top position. Now remaining is the endpoint calibration
				// (adjusting for the backward movement because of curtain tension)
				status = CalibratingEndPoint;
				sensor_ticks_while_calibrating_endpoint = 0;	// for debugging
				// now we wait until curtain rod stabilizes
				endpoint_calibration_started_timestamp = HAL_GetTick();
			} else {
				// motor should not stall when direction is down!
				status = Error;
			}
		} else if (current_status == Stopping) {
			// Motor was accidently stalled during slowing down
			status = Stopped;
		}
	}
}


void motor_stop() {

	// Make sure that all mosfets are off
	pwm_stop(LOW1_PWM_CHANNEL);
	pwm_stop(LOW2_PWM_CHANNEL);
	/*
	 * Remember to double-check that the code generated in HAL_TIM_MspPostInit by CubeMX has GPIO_PULLDOWN setting
	 * enabled for LOW_1_GATE_Pin and LOW_2_GATE_Pin !!
	 */

	HAL_GPIO_WritePin(HIGH_1_GATE_GPIO_Port, HIGH_1_GATE_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(HIGH_2_GATE_GPIO_Port, HIGH_2_GATE_Pin, GPIO_PIN_RESET);
	TIM1->CCR1 = 0;
	TIM1->CCR4 = 0;
	status = Stopped;
	direction = None;
	curr_pwm = 0;

	// for debugging
	sensor_ticks_while_stopped = 0;
	saved_hall_sensor_1_ticks = hall_sensor_1_ticks;
	saved_hall_sensor_2_ticks = hall_sensor_2_ticks;

	// reset stall detection timeout
	hall_sensor_1_interval = 0;
	hall_sensor_1_ticks = 0;
	hall_sensor_2_ticks = 0;
	hall_sensor_1_idle_time = 0;
	target_speed = 0;

}


void motor_start_common(uint8_t motor_speed) {
	motor_stop();	// first reset all the settings just in case..
	HAL_Delay(10);
	movement_started_timestamp = HAL_GetTick();

	target_speed = motor_speed;
	curr_pwm = INITIAL_PWM;
	status = Moving;
}

void motor_up(uint8_t motor_speed) {

	motor_start_common(motor_speed);

	// turn on LOW2 PWM and HIGH1
	pwm_start(LOW2_PWM_CHANNEL);
	TIM1->CCR4 = INITIAL_PWM;
	HAL_GPIO_WritePin(HIGH_1_GATE_GPIO_Port, HIGH_1_GATE_Pin, GPIO_PIN_SET);
	direction = Up;

}

void motor_down(uint8_t motor_speed) {

	motor_start_common(motor_speed);

	// turn on LOW1 PWM and HIGH2
	pwm_start(LOW1_PWM_CHANNEL);
	TIM1->CCR1 = INITIAL_PWM;
	HAL_GPIO_WritePin(HIGH_2_GATE_GPIO_Port, HIGH_2_GATE_Pin, GPIO_PIN_SET);
	direction = Down;
}


#ifndef SLIM_BINARY
uint8_t check_voltage() {
	if (minimum_voltage != 0) {
		uint16_t voltage = get_voltage() / 30;
		if (voltage < minimum_voltage)
			return 0;
	}
	return 1;
}
#endif

void motor_process() {
	if (command == MotorUp) {
		motor_up(default_speed);
		command = NoCommand;
	} else if (command == MotorDown) {
		motor_down(default_speed);
		command = NoCommand;
	} else if(command == Stop) {
		motor_stop();
		command = NoCommand;
	}
}

#ifndef SLIM_BINARY
uint8_t calculate_battery() {
	return 0x12; // TODO
}
#endif


uint8_t handle_command(uint8_t * rx_buffer, uint8_t * tx_buffer, uint8_t burstindex, uint8_t * tx_bytes) {
	uint8_t cmd1, cmd2;
	cmd1 = rx_buffer[3];
	cmd2 = rx_buffer[4];
	uint16_t cmd = (cmd1 << 8) + cmd2;

	uint8_t cmd_handled = 1;

	switch (cmd) {

		case CMD_GET_STATUS:
			{
				tx_buffer[2] = 0xd8;
#ifndef SLIM_BINARY
				tx_buffer[3] = calculate_battery();
#else
				tx_buffer[3] = 0x12;
#endif
				tx_buffer[4] = (uint8_t)( get_voltage()/16);  // returned value is voltage*30
				tx_buffer[5] = (uint8_t)get_rpm();
				tx_buffer[6] = location_to_position100();
				tx_buffer[7] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6];
				*tx_bytes=8;
			}
			break;

		case CMD_UP:
			{
				target_location = -1;	// motor goes up until it stalls which forces calibration
				command = MotorUp;
			}
			break;

		case CMD_DOWN:
			{
				target_location = max_curtain_length;
				command = MotorDown;
			}
			break;

		case CMD_UP_17:
			{
				target_location = location - DEG_TO_LOCATION(17);
				if (target_location < 0)
					target_location = 0;
				command = MotorUp;
			}
			break;

		case CMD_DOWN_17:
			{
				target_location = location + DEG_TO_LOCATION(17);
				if (target_location > max_curtain_length)
					target_location = max_curtain_length;
				command = MotorDown;
			}
			break;

		case CMD_STOP:
			{
				command = Stop;
			}
			break;

		case CMD_OVERRIDE_UP_90:
			{
				target_location = location - DEG_TO_LOCATION(90);
				command = MotorUp;
			}
			break;

		case CMD_OVERRIDE_DOWN_90:
			{
				target_location = location + DEG_TO_LOCATION(90);
				command = MotorDown;
			}
			break;

		case CMD_OVERRIDE_UP_6:
			{
				target_location = location - DEG_TO_LOCATION(6);
				command = MotorUp;
			}
			break;

		case CMD_OVERRIDE_DOWN_6:
			{
				target_location = location + DEG_TO_LOCATION(6);
				command = MotorDown;
			}
			break;
		case CMD_SET_FULL_CURTAIN_LENGTH:
			{
				motor_write_setting(FULL_CURTAIN_LEN_EEPROM, location);
				full_curtain_length = location;
			}
			// fall-through: maximum curtain length will be reset as well

		case CMD_SET_MAX_CURTAIN_LENGTH:
			{
				motor_write_setting(MAX_CURTAIN_LEN_EEPROM, location);
				max_curtain_length = location;
			}
			break;

		case CMD_RESET_CURTAIN_LENGTH:
			{
				motor_write_setting(MAX_CURTAIN_LEN_EEPROM, full_curtain_length);
				max_curtain_length = full_curtain_length;
				calibrating = 1;	// allow unrestricted movement until the end of calibration
			}
			break;
		case CMD_EXT_OVERRIDE_DOWN:
			{
				target_location = location + DEG_TO_LOCATION(360*5);
				command = MotorDown;
			}
			break;
		case CMD_EXT_GET_VERSION:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xd0;
				tx_buffer[3] = VERSION_MAJOR;
				tx_buffer[4] = VERSION_MINOR;
				tx_buffer[5] = minimum_voltage;
				tx_buffer[6] = default_speed;
				tx_buffer[7] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6];
				*tx_bytes=8;
			}
			break;
			case CMD_EXT_DEBUG:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xd2;
				tx_buffer[3] = 0;
				tx_buffer[4] = (uint8_t)dir_error;
				tx_buffer[5] = (uint8_t)sensor_ticks_while_calibrating_endpoint;
				tx_buffer[6] = (uint8_t)sensor_ticks_while_stopped;
				tx_buffer[7] = 0;
				tx_buffer[8] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6] ^ tx_buffer[7];
				*tx_bytes=9;
			}
			break;
		case CMD_EXT_SENSOR_DEBUG:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xd3;
				tx_buffer[3] = saved_hall_sensor_1_ticks >> 8;
				tx_buffer[4] = saved_hall_sensor_1_ticks & 0xff;
				tx_buffer[5] = saved_hall_sensor_2_ticks >> 8;
				tx_buffer[6] = saved_hall_sensor_2_ticks & 0xff;
				tx_buffer[7] = 0;
				tx_buffer[8] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6] ^ tx_buffer[7];
				*tx_bytes=9;
			}
			break;
		case CMD_EXT_GET_LOCATION:
			{
				tx_buffer[2] = 0xd1;
				tx_buffer[3] = location >> 8;
				tx_buffer[4] = location & 0xff;
				tx_buffer[5] = target_location >> 8;
				tx_buffer[6] = target_location & 0xff;
				tx_buffer[7] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6];
				*tx_bytes=8;
			}
			break;

		case CMD_EXT_GET_STATUS:
			{
				tx_buffer[2] = 0xda;
				tx_buffer[3] = status;
#ifndef SLIM_BINARY
				tx_buffer[4] = (uint8_t)(get_motor_current());
#else
				tx_buffer[4] = 0;
#endif
				tx_buffer[5] = (uint8_t)get_rpm();
				float pos2 = location_to_position100() * 256;
				int pos = pos2;
				tx_buffer[6] = pos >> 8;
				tx_buffer[7] = pos & 0xff;
				tx_buffer[8] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6] ^ tx_buffer[7];
				*tx_bytes=9;
			}
			break;
		case CMD_EXT_GET_LIMITS:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xdb;
				tx_buffer[3] = calibrating;
				tx_buffer[4] = max_curtain_length >> 8;
				tx_buffer[5] = max_curtain_length & 0xff;
				tx_buffer[6] = full_curtain_length >> 8;
				tx_buffer[7] = full_curtain_length & 0xff;
				tx_buffer[8] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6] ^ tx_buffer[7];
				*tx_bytes=9;
			}
			break;
		default:
			cmd_handled=0;
	}

	if (!cmd_handled) {
		// one byte commands with parameter
		if (cmd1 == CMD_EXT_SET_SPEED) {
			if (cmd2 > 1) {
				default_speed = cmd2;
				if (target_speed != 0)
					target_speed = cmd2;
			}
		} else if (cmd1 == CMD_EXT_SET_DEFAULT_SPEED) {
			if (cmd2 > 0) {
				motor_write_setting(DEFAULT_SPEED_EEPROM, cmd2);
				default_speed = cmd2;
			}
		} else if (cmd1 == CMD_GO_TO) {
			if (!calibrating) {
				target_location = position100_to_location(cmd2);
				if (target_location < location) {
					command = MotorUp;
				} else {
					command = MotorDown;
				}
			}
		} else if ((cmd1 & 0xf0) == CMD_EXT_GO_TO) {
			if (!calibrating) {
				uint16_t pos = ((cmd1 & 0x0f)<<8) + cmd2;
				float pos2 = ((float)pos)/16;
				target_location = position100_to_location(pos2);
				if (target_location < location) {
					command = MotorUp;
				} else {
					command = MotorDown;
				}
			}
		} else if ((cmd1 & 0xf0) == CMD_EXT_SET_LOCATION) {
			// There is only room for 12 bits of data, so we have omitted 1 least-significant bit
			uint16_t loc = (((cmd1 & 0x0f)<<8) + cmd2) << 1;
			location = loc;
			calibrating = 0;
		} else if (cmd1 == CMD_EXT_SET_MINIMUM_VOLTAGE) {
			motor_write_setting(MINIMUM_VOLTAGE_EEPROM, cmd2);
			minimum_voltage = cmd2;
		} else if (cmd1 == CMD_EXT_SET_AUTO_CAL) {
			motor_write_setting(AUTO_CAL_EEPROM, cmd2);
			auto_calibration = cmd2;
		} else if ((cmd1 & 0xf0) == CMD_EXT_GO_TO_LOCATION) {
			// There is only room for 12 bits of data, so we have omitted 1 least-significant bit
			target_location = (((cmd1 & 0x0f)<<8) + cmd2) << 1;
			if (target_location < location) {
				command = MotorUp;
			} else {
				command = MotorDown;
			}
		} else if (cmd1 == CMD_EXT_SET_SLOWDOWN_FACTOR) {
			slowdown_factor = cmd2;
		} else if (cmd1 == CMD_EXT_SET_MIN_SLOWDOWN_SPEED) {
			min_slowdown_speed = cmd2;
		}
	}



	return 1;
}

void motor_init() {

	motor_stop();

	location = max_curtain_length; // assume we are at bottom position

	if (auto_calibration) {
		calibrating = 1;
		command = MotorUp;
	} else {
		calibrating = 0;
		command = NoCommand;
	}
}




