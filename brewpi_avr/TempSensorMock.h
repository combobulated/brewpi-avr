/*
 * Copyright 2012-2013 BrewPi/Elco Jacobs.
 * Copyright 2013 Matthew McGowan.
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

#pragma once

#include "Brewpi.h"
#include "TempSensor.h"

class MockTempSensor : public BasicTempSensor
{
public:	
	MockTempSensor(fixed7_9 initial, fixed7_9 delta) : _temperature(initial), _delta(delta), _connected(true) { }
	
	void setConnected(bool connected)
	{
		_connected = connected;
	}
	
	bool isConnected() { return _connected; }

	fixed7_9 init() {
		return read();
	}
	
	fixed7_9 read()
	{
		if (!isConnected())
			return DEVICE_DISCONNECTED;
		
		switch (tempControl.getMode()) {
			case COOLING:
				_temperature -= _delta;
				break;
			case HEATING:
				_temperature += _delta;
				break;
		}
		
		return _temperature;
	}
	
	private:
	fixed7_9 _temperature;	
	fixed7_9 _delta;	
	bool _connected;
};

