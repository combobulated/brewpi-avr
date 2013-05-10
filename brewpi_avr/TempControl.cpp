/*
 * Copyright 2012 BrewPi/Elco Jacobs.
 *
 * This file is part of BrewPi.
 * 
 * BrewPi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * BrewPi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with BrewPi.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "brewpi_avr.h"

#include "pins.h"
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <limits.h>

#include "temperatureFormats.h"
#include "TempControl.h"
#include "PiLink.h"
#include "TempSensor.h"
#include "Ticks.h"
#include "chamber.h"
#include "MockTempSensor.h"
#include "EepromManager.h"
#include "ExternalTempSensor.h"

TempControl tempControl;

#if TEMP_CONTROL_STATIC

extern ValueSensor<bool> defaultSensor;
extern ValueActuator defaultActuator;
extern ExternalTempSensor defaultTempSensor;

// These sensors are switched out to implement multi-chamber.
TempSensor* TempControl::beerSensor;
TempSensor* TempControl::fridgeSensor;
BasicTempSensor* TempControl::ambientSensor = &defaultTempSensor;

Actuator* TempControl::heater = &defaultActuator;
Actuator* TempControl::cooler = &defaultActuator;
Actuator* TempControl::light = &defaultActuator;
Sensor<bool>* TempControl::door = &defaultSensor;
	
// Control parameters
ControlConstants TempControl::cc;
ControlSettings TempControl::cs;
ControlVariables TempControl::cv;
	
	// State variables
uint8_t TempControl::state;
bool TempControl::doPosPeakDetect;
bool TempControl::doNegPeakDetect;
	
	// keep track of beer setting stored in EEPROM
fixed7_9 TempControl::storedBeerSetting;
	
	// Timers
unsigned int TempControl::lastIdleTime;
unsigned int TempControl::lastHeatTime;
unsigned int TempControl::lastCoolTime;
#endif



void TempControl::updateState(void){
	//update state
	if(door->sense()){
		if(state!=DOOR_OPEN){
			// todo - use place holder annotation strings, and replace with full message in the python script. 
			piLink.printFridgeAnnotation(PSTR("Fridge door opened"));
		}
		state=DOOR_OPEN;
		return;
	}
	if(cs.mode == MODE_OFF){
		state = STATE_OFF;
		return;
	}
	if(cs.fridgeSetting == INT_MIN){
		// Don nothing when fridge setting is undefined
		state = IDLE;
		return;
	}
	
	if(!fridgeSensor->isConnected() || (!beerSensor->isConnected() && (cs.mode == MODE_BEER_CONSTANT || cs.mode == MODE_BEER_PROFILE))){
		state = IDLE; // stay idle when one of the sensors is disconnected
		return;
	}

	uint16_t sinceIdle = timeSinceIdle();
	uint16_t sinceCooling = timeSinceCooling();
	uint16_t sinceHeating = timeSinceHeating();
	fixed7_9 fridgeFast = fridgeSensor->readFastFiltered();
	fixed7_9 beerFast = beerSensor->readFastFiltered();
	ticks_seconds_t secs = ticks.seconds();
	switch(state)
	{
		case STARTUP:
		case IDLE:
		case STATE_OFF:
		{
			lastIdleTime=secs;
			if(doNegPeakDetect == true || doPosPeakDetect == true){
				// Wait for peaks before starting to heat or cool again
					return;
			}		  
			if(fridgeFast > (cs.fridgeSetting+cc.idleRangeHigh) ){ // fridge temperature is too high
				if(cs.mode==MODE_FRIDGE_CONSTANT){
					if((sinceCooling > MIN_COOL_OFF_TIME_FRIDGE_CONSTANT && sinceHeating > MIN_SWITCH_TIME) || state == STARTUP){
						state=COOLING;
					}
					return;
				}
			else{
				if(beerFast<cs.beerSetting){ // only start cooling when beer is too warm
						return; // beer is already colder than setting, stay in IDLE.
				}
				if((sinceCooling > MIN_COOL_OFF_TIME && sinceHeating > MIN_SWITCH_TIME) || state == STARTUP){
					state=COOLING;
				}
				return;
			}
			}
			else if(fridgeFast < (cs.fridgeSetting+cc.idleRangeLow)){ // fridge temperature is too low
				if(beerFast >cs.beerSetting){ // only start heating when beer is too cold
					return; // beer is already warmer than setting, stay in IDLE
			}
				if((sinceCooling > MIN_SWITCH_TIME && sinceHeating > MIN_HEAT_OFF_TIME) || state == STARTUP){
					state=HEATING;
					return;
		}			
			}
		}			
		break; 
		case COOLING:
		{
			doNegPeakDetect=true;
			lastCoolTime = secs;
			updateEstimatedPeak(cc.maxCoolTimeForEstimate, cs.coolEstimator, sinceIdle);
			if(cv.estimatedPeak <= cs.fridgeSetting){
				if(sinceIdle > MIN_COOL_ON_TIME){
					cv.negPeakEstimate = cv.estimatedPeak; // remember estimated peak when I switch to IDLE, to adjust estimator later
					state=IDLE;
				}					
			}
		}		
		break;
		case HEATING:
		{
			doPosPeakDetect=true;
			lastHeatTime=secs;
			updateEstimatedPeak(cc.maxHeatTimeForEstimate, cs.heatEstimator, sinceIdle);
			if(cv.estimatedPeak >= cs.fridgeSetting){
				if(sinceIdle > MIN_HEAT_ON_TIME){
					cv.posPeakEstimate=cv.estimatedPeak; // remember estimated peak when I switch to IDLE, to adjust estimator later
					state=IDLE;
				}
			}
		}
		break;
		case DOOR_OPEN:
		{
			if(!door->sense()){ 
				piLink.printFridgeAnnotation(PSTR("Fridge door closed"));
				state=IDLE;
			}
		}
		break;
	}			
}

void TempControl::updateEstimatedPeak(uint16_t time, fixed7_9 estimator, uint16_t sinceIdle)
{
	uint16_t activeTime = min(time, sinceIdle); // heat time in seconds
	fixed7_9 estimatedOvershoot = ((fixed23_9) estimator * activeTime)/3600; // overshoot estimator is in overshoot per hour
	cv.estimatedPeak = fridgeSensor->readFastFiltered() + estimatedOvershoot;		
}

void TempControl::updateOutputs(void) {
	if (cs.mode==MODE_TEST)
		return;
	
	cooler->setActive(state==COOLING);	
#if LIGHT_AS_HEATER
	heater->setActive(state==DOOR_OPEN || state==HEATING);
#else
	heater->setActive(state==HEATING);
	light->setActive(state==DOOR_OPEN);
#endif		
// todo - factor out doorOpen state so it is independent of the temp control state. That way, opening/closing the door doesn't affect compressor operation.
}




uint8_t TempControl::storeConstants(eptr_t offset){	
	eepromAccess.writeBlock(offset, (void *) &cc, sizeof(ControlConstants));
	return sizeof(ControlConstants);
}

uint8_t TempControl::loadConstants(eptr_t offset){
	eepromAccess.readBlock((void *) &cc, offset, sizeof(ControlConstants));
	constantsChanged();
	return sizeof(ControlConstants);
}

// write new settings to EEPROM to be able to reload them after a reset
// The update functions only write to EEPROM if the value has changed
uint8_t TempControl::storeSettings(eptr_t offset){
	eepromAccess.writeBlock(offset, (void *) &cs, sizeof(ControlSettings));
	storedBeerSetting = cs.beerSetting;	
	return sizeof(ControlSettings);
}

uint8_t TempControl::loadSettings(eptr_t offset){
	eepromAccess.readBlock((void *) &cs, offset, sizeof(ControlSettings));	
	return sizeof(ControlSettings);
}


void TempControl::loadDefaultConstants(void){
	cc.tempFormat = 'C';
	// maximum history to take into account, in seconds
	cc.maxHeatTimeForEstimate = 600;
	cc.maxCoolTimeForEstimate = 1200;

	// Limits of fridge temperature setting
	cc.tempSettingMax = 30*512;	// +30 deg Celsius
	cc.tempSettingMin = 1*512;	// +1 deg Celsius

	// control defines, also in fixed point format (7 int bits, 9 frac bits), so multiplied by 2^9=512
	cc.Kp	= 10240;	// +20
	cc.Ki		= 307;		// +0.6
	cc.Kd	= -1536;	// -3
	cc.iMaxError = 256;  // 0.5 deg

	// Stay Idle when temperature is in this range
	cc.idleRangeHigh = 512;	// +1 deg Celsius
	cc.idleRangeLow = -512;	// -1 deg Celsius

	// when peak falls between these limits, its good.
	cc.heatingTargetUpper = 154;	// +0.3 deg Celsius
	cc.heatingTargetLower = -102;	// -0.2 deg Celsius
	cc.coolingTargetUpper = 102;	// +0.2 deg Celsius
	cc.coolingTargetLower = -154;	// -0.3 deg Celsius

	// Set filter coefficients. This is the b value. See FixedFilter.h for delay times.
	// The delay time is 3.33 * 2^b * number of cascades
	cc.fridgeFastFilter = 1u;
	cc.fridgeSlowFilter = 4u;
	cc.fridgeSlopeFilter = 3u;
	cc.beerFastFilter = 3u;
	cc.beerSlowFilter = 5u;
	cc.beerSlopeFilter = 4u;
	constantsChanged();
}

void TempControl::constantsChanged()
{
	fridgeSensor->setFastFilterCoefficients(cc.fridgeFastFilter);
	fridgeSensor->setSlowFilterCoefficients(cc.fridgeSlowFilter);
	fridgeSensor->setSlopeFilterCoefficients(cc.fridgeSlopeFilter);
	beerSensor->setFastFilterCoefficients(cc.beerFastFilter);
	beerSensor->setSlowFilterCoefficients(cc.beerSlowFilter);
	beerSensor->setSlopeFilterCoefficients(cc.beerSlopeFilter);	
}

#if 0
// this is only called during startup - move into SettingsManager
void TempControl::loadSettingsAndConstants(void){
	uint16_t offset = EEPROM_CONTROL_BLOCK_SIZE*CURRENT_CHAMBER+EEPROM_TC_SETTINGS_BASE_ADDRESS;
	if(eeprom_read_byte((unsigned char*) offset) != 1){
		// EEPROM is not initialized, use default settings
		loadDefaultSettings();
		loadDefaultConstants();
		eeprom_write_byte((unsigned char *) offset, 1);
	}
	else{
		loadSettings();
		loadConstants();
	}
}
#endif

void TempControl::setMode(char newMode){
	if(newMode != cs.mode){
		state = IDLE;
	cs.mode = newMode;
	if(newMode==MODE_BEER_PROFILE || newMode == MODE_OFF){
		// set temperatures to undefined until temperatures have been received from RPi
		cs.beerSetting = INT_MIN;
		cs.fridgeSetting = INT_MIN;
	}
	eepromManager.storeTempSettings();
	}
}

fixed7_9 TempControl::getBeerTemp(void){
	return beerSensor->readFastFiltered();
}

fixed7_9 TempControl::getBeerSetting(void){
	return cs.beerSetting;	
}

fixed7_9 TempControl::getFridgeTemp(void){
	return fridgeSensor->readFastFiltered();	
}

fixed7_9 TempControl::getFridgeSetting(void){
	return cs.fridgeSetting;	
}

void TempControl::setBeerTemp(int newTemp){
	int oldBeerSetting = cs.beerSetting;
	cs.beerSetting= newTemp;
	if(abs(oldBeerSetting - newTemp) > 128){ // more than half a degree C difference with old setting
		reset(); // reset controller
	}
	updatePID();
	updateState();
	if(abs(storedBeerSetting - newTemp) > 128){ // more than half a degree C difference with EEPROM
		// Do not store settings every time, because EEPROM has limited number of write cycles.
		// If Raspberry Pi is connected, it will update the settings anyway. This is just a safety feature.
		eepromManager.storeTempSettings();
	}		
}

// todo - this method is called many times while manually changing the fridge temp using the rotary encoder.
// Seems to hit the eeprom quite a bit.
void TempControl::setFridgeTemp(int newTemp){
	cs.fridgeSetting = newTemp;
	reset(); // reset peak detection and PID
	updatePID();
	updateState();	
}

