#ifndef __WCLOCK_H__
#define __WCLOCK_H__

#include "Arduino.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Time.h>
#include <TimeLib.h>
#include "WDevice.h"
#include "WNetwork.h"

const char* DEFAULT_NTP_SERVER = "pool.ntp.org";
const char* DEFAULT_TIME_ZONE_SERVER = "http://worldtimeapi.org/api/ip";
const static char HTTP_CLOCK_CONFIG_PAGE_STYLE[]    PROGMEM = R"=====(
	<style>
	#div_tz {
	  display:%s;
	}
	#div_ro {
	  display:%s;
	}
	</style>
)=====";
const static char HTTP_CLOCK_CONFIG_PAGE[]    PROGMEM = R"=====(
	<div>
		NTP server:<br>
		<input type='text' name='ntp' length=32 value='%s'>
	</div>

	<div>
		<label>
			<input type='radio' id='r_tz' name='ts' value='true' %s onclick='toggleTzGroup()'>
			Evaluate time zone from internet
		</label>
		<label>
			<input type='radio' id='r_ro' name='ts' value='false' %s onclick='toggleTzGroup()'>
			Use fixed time offset to NTP-UTC time
		</label>
	</div>
	<div id ='div_tz'>
		Time zone server:<br>
		<input type='text' name='tz' length=64 value='%s'>
	</div>
	<div id ='div_ro'>
		Fixed offset in minutes:<br>
		<input type='text' name='ro' length=10 value='%s'>
	</div>
	<script>
		function toggleTzGroup() {
			var rro = document.getElementById('r_ro');
  		var dtz = document.getElementById('div_tz');
			var dro = document.getElementById('div_ro');
  		if (rro.checked) {
	    	dtz.style.display = 'none';
				dro.style.display = 'block';
  		} else {
				dtz.style.display = 'block';
				dro.style.display = 'none';
  		}
		}
	</script>
)=====";

class WClock: public WDevice {
public:
	typedef std::function<void(void)> THandlerFunction;
	typedef std::function<void(const char*)> TErrorHandlerFunction;



	//WClock(bool debug, WNetwork *network) {
	WClock(WNetwork* network, String applicationName)
		: WDevice(network, "clock", "clock", DEVICE_TYPE_TEXT_DISPLAY) {
		this->mainDevice = false;
		this->visibility = MQTT;
		this->ntpServer = network->getSettings()->setString("ntpServer", 32, DEFAULT_NTP_SERVER);
		this->ntpServer->setReadOnly(true);
		this->ntpServer->setVisibility(MQTT);
		this->addProperty(ntpServer);
		this->useTimeZoneServer = network->getSettings()->setBoolean("useTimeZoneServer", true);
		this->useTimeZoneServer->setReadOnly(true);
		this->useTimeZoneServer->setVisibility(NONE);
		this->addProperty(useTimeZoneServer);
		this->timeZoneServer = network->getSettings()->setString("timeZoneServer", 64, DEFAULT_TIME_ZONE_SERVER);
		this->timeZoneServer->setReadOnly(true);
		this->timeZoneServer->setVisibility(this->useTimeZoneServer->getBoolean() ? MQTT : NONE);
		//this->ntpServer->setVisibility(MQTT);
		this->addProperty(timeZoneServer);
		this->epochTime = new WUnsignedLongProperty("epochTime");
		this->epochTime->setReadOnly(true);
		this->epochTime->setOnValueRequest([this](WProperty* p) {
			p->setUnsignedLong(getEpochTime());
		});
		this->addProperty(epochTime);
		this->epochTimeFormatted = new WStringProperty("epochTimeFormatted", "epochTimeFormatted", 19);
		this->epochTimeFormatted->setReadOnly(true);
		this->epochTimeFormatted->setOnValueRequest([this](WProperty* p) {updateFormattedTime();});
		this->addProperty(epochTimeFormatted);
		this->validTime = new WOnOffProperty("validTime", "validTime");
		this->validTime->setBoolean(false);
		this->validTime->setReadOnly(true);
		this->addProperty(validTime);
		this->timeZone = new WStringProperty("timezone", "timeZone", 32);
		this->timeZone->setReadOnly(true);
		this->addProperty(timeZone);
		this->rawOffset = new WLongProperty("raw_offset", "rawOffset");
		this->rawOffset->setLong(0);
		this->network->getSettings()->add(this->rawOffset);
		this->rawOffset->setReadOnly(true);
		this->addProperty(rawOffset);
		this->dstOffset = new WLongProperty("dst_offset", "dstOffset");
		this->dstOffset->setLong(0);
		this->dstOffset->setReadOnly(true);
		this->addProperty(dstOffset);
		lastTry = lastNtpSync = lastTimeZoneSync = ntpTime = 0;
	}

	void loop(unsigned long now) {
		//Invalid after 3 hours
		validTime->setBoolean((lastNtpSync > 0) && (lastTimeZoneSync > 0) && (now - lastTry < (3 * 60 * 60000)));

		if (((!isValidTime()) && ((lastTry == 0) || (now - lastTry > 60000)))
				&& (WiFi.status() == WL_CONNECTED)) {
			//1. Sync ntp
			if ((lastNtpSync == 0) || (now - lastNtpSync > 60000)) {
				network->log()->notice(F("Time via NTP server '%s'"), ntpServer->c_str());
				WiFiUDP ntpUDP;
				NTPClient ntpClient(ntpUDP, ntpServer->c_str());
				//ntpClient.begin();
				//delay(500);
				if (ntpClient.update()) {
					lastNtpSync = millis();
					ntpTime = ntpClient.getEpochTime();
					network->log()->notice(F("NTP time synced: %s"), epochTimeFormatted->c_str());
					notifyOnTimeUpdate();
				} else {
					notifyOnError("NTP sync failed. ");
				}
			}
			//2. Sync time zone
			if (((lastNtpSync > 0) && ((lastTimeZoneSync == 0) || (now - lastTimeZoneSync > 60000)))
					&& (WiFi.status() == WL_CONNECTED)
					&& (useTimeZoneServer->getBoolean())
				  && (!timeZoneServer->equalsString(""))) {
				String request = timeZoneServer->c_str();
				network->log()->notice(F("Time zone update via '%s'"), request.c_str());
				HTTPClient http;
				http.begin(request);
				int httpCode = http.GET();
				if (httpCode > 0) {
					WJsonParser parser;
					this->timeZone->setReadOnly(false);
					this->rawOffset->setReadOnly(false);
					this->dstOffset->setReadOnly(false);
					WProperty* property = parser.parse(http.getString().c_str(), this);
					this->timeZone->setReadOnly(true);
					this->rawOffset->setReadOnly(true);
					this->dstOffset->setReadOnly(true);
					if (property != nullptr) {
						lastTimeZoneSync = millis();
						validTime->setBoolean(true);
						network->log()->notice(F("Time zone evaluated. Current local time: %s"), epochTimeFormatted->c_str());
						notifyOnTimeUpdate();
												 /* {
												 *  "week_number":19,
												 *  "utc_offset":"+09:00",
												 *  "utc_datetime":"2019-05-07T23:32:15.214725+00:00",
												 *  "unixtime":1557271935,
												 *  "timezone":"Asia/Seoul",
												 *  "raw_offset":32400,
												 *  "dst_until":null,
												 *  "dst_offset":0,
												 *  "dst_from":null,
												 *  "dst":false,
												 *  "day_of_year":128,
												 *  "day_of_week":3,
												 *  "datetime":"2019-05-08T08:32:15.214725+09:00",
												 *  "abbreviation":"KST"}
												 */
												/*
												 * {"week_number":19,
												 *  "utc_offset":"+02:00",
												 *  "utc_datetime":"2019-05-07T23:37:41.963463+00:00",
												 *  "unixtime":1557272261,
												 *  "timezone":"Europe/Berlin",
												 *  "raw_offset":7200,
												 *  "dst_until":"2019-10-27T01:00:00+00:00",
												 *  "dst_offset":3600,
												 *  "dst_from":"2019-03-31T01:00:00+00:00",
												 *  "dst":true,
												 *  "day_of_year":128,
												 *  "day_of_week":3,
												 *  "datetime":"2019-05-08T01:37:41.963463+02:00",
												 *  "abbreviation":"CEST"}
												 */
					}

				} else {
					notifyOnError("Time zone update failed: " + httpCode);
				}
				http.end();   //Close connection
			}
			lastTry = millis();
		}
	}

	void setOnTimeUpdate(THandlerFunction onTimeUpdate) {
		this->onTimeUpdate = onTimeUpdate;
	}

	void setOnError(TErrorHandlerFunction onError) {
		this->onError = onError;
	}

	unsigned long getEpochTime() {
		return (lastNtpSync > 0 ? ntpTime + getRawOffset() + getDstOffset() + ((millis() - lastNtpSync) / 1000) :	0);
	}

	byte getWeekDay() {
		return getWeekDay(getEpochTime());
	}

	byte getWeekDay(unsigned long epochTime) {
		//weekday from 0 to 6, 0 is Sunday
		return (((epochTime / 86400L) + 4) % 7);
	}

	byte getHours() {
		return getHours(getEpochTime());
	}

	byte getHours(unsigned long epochTime) {
		return ((epochTime % 86400L) / 3600);
	}

	byte getMinutes() {
		return getMinutes(getEpochTime());
	}

	byte getMinutes(unsigned long epochTime) {
		return ((epochTime % 3600) / 60);
	}

	byte getSeconds() {
		return getSeconds(getEpochTime());
	}

	byte getSeconds(unsigned long epochTime) {
		return (epochTime % 60);
	}

	int getYear() {
		return getYear(getEpochTime());
	}

	int getYear(unsigned long epochTime) {
		return year(epochTime);
	}

	byte getMonth() {
		return getMonth(getEpochTime());
	}

	byte getMonth(unsigned long epochTime) {
		//month from 1 to 12
		return month(epochTime);
	}

	byte getDay() {
		return getDay(getEpochTime());
	}

	byte getDay(unsigned long epochTime) {
		//day from 1 to 31
		return day(epochTime);
	}

	bool isTimeLaterThan(byte hours, byte minutes) {
		return ((getHours() > hours) || ((getHours() == hours) && (getMinutes() >= minutes)));
	}

	bool isTimeEarlierThan(byte hours, byte minutes) {
		return ((getHours() < hours) || ((getHours() == hours) && (getMinutes() < minutes)));
	}

	bool isTimeBetween(byte hours1, byte minutes1, byte hours2, byte minutes2) {
		return ((isTimeLaterThan(hours1, minutes1)) && (isTimeEarlierThan(hours2, minutes2)));
	}

	void updateFormattedTime() {
		updateFormattedTimeImpl(getEpochTime());
	}

	void updateFormattedTimeImpl(unsigned long rawTime) {
		WStringStream* stream = new WStringStream(19);
		char buffer[5];
		//year
		int _year = year(rawTime);
		itoa(_year, buffer, 10);
		stream->print(buffer);
		stream->print("-");
		//month
		uint8_t _month = month(rawTime);
		if (_month < 10) stream->print("0");
		itoa(_month, buffer, 10);
		stream->print(buffer);
		stream->print("-");
		//month
		uint8_t _day = day(rawTime);
		if (_day < 10) stream->print("0");
		itoa(_day, buffer, 10);
		stream->print(buffer);
		stream->print(" ");
		//hours
		unsigned long _hours = (rawTime % 86400L) / 3600;
		if (_hours < 10) stream->print("0");
		itoa(_hours, buffer, 10);
		stream->print(buffer);
		stream->print(":");
		//minutes
		unsigned long _minutes = (rawTime % 3600) / 60;
		if (_minutes < 10) stream->print("0");
		itoa(_minutes, buffer, 10);
		stream->print(buffer);
		stream->print(":");
		//seconds
		unsigned long _seconds = rawTime % 60;
		if (_seconds < 10) stream->print("0");
		itoa(_seconds, buffer, 10);
		stream->print(buffer);

		epochTimeFormatted->setString(stream->c_str());
		delete stream;
	}

	bool isValidTime() {
		return validTime->getBoolean();
	}

	bool isClockSynced() {
		return ((lastNtpSync > 0) && (lastTimeZoneSync > 0));
	}

	long getRawOffset() {
		return rawOffset->getLong();
	}

	long getDstOffset() {
		return dstOffset->getLong();
	}



	void printConfigPage(WStringStream* page) {
    	network->log()->notice(F("Clock config page"));
    	page->printAndReplace(FPSTR(HTTP_CONFIG_PAGE_BEGIN), getId());
			page->printAndReplace(FPSTR(HTTP_CLOCK_CONFIG_PAGE_STYLE), (useTimeZoneServer->getBoolean() ? "block" : "none"), (useTimeZoneServer->getBoolean() ? "none" : "block"));
    	page->printAndReplace(FPSTR(HTTP_CLOCK_CONFIG_PAGE), ntpServer->c_str(),
																													 (useTimeZoneServer->getBoolean() ? "checked" : ""), (useTimeZoneServer->getBoolean() ? "" : "checked"),
			                                                     timeZoneServer->c_str(),
																												   String(getRawOffset() / 60).c_str());
    	page->print(FPSTR(HTTP_CONFIG_SAVE_BUTTON));
	}

	void saveConfigPage(ESP8266WebServer* webServer) {
		network->log()->notice(F("Save clock config page"));
		this->ntpServer->setString(webServer->arg("ntp").c_str());
		this->timeZoneServer->setString(webServer->arg("tz").c_str());
		this->useTimeZoneServer->setBoolean(webServer->arg("ts") == "true");
		this->rawOffset->setLong(atoi(webServer->arg("ro").c_str()) * 60);
	}

private:
	THandlerFunction onTimeUpdate;
	TErrorHandlerFunction onError;
	unsigned long lastTry, lastNtpSync, lastTimeZoneSync, ntpTime;
	WProperty* epochTime;
	WProperty* epochTimeFormatted;
	WProperty* validTime;
	WProperty* ntpServer;
	WProperty* useTimeZoneServer;
	WProperty* timeZoneServer;
	WProperty* timeZone;
	WProperty* rawOffset;
	WProperty* dstOffset;

	void notifyOnTimeUpdate() {
		if (onTimeUpdate) {
			onTimeUpdate();
		}
	}

	void notifyOnError(const char* error) {
		network->log()->notice(error);
		if (onError) {
			onError(error);
		}
	}
};

#endif