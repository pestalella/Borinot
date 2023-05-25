#include "passwords.h"

#include "Adafruit_Sensor.h"
// #include <Adafruit_BME280.h>
#include "Adafruit_AM2320.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_mac.h"
#include <Wire.h>

#include <WiFiMulti.h>
WiFiMulti wifiMulti;
#define DEVICE "ESP32"
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// Time zone info
#define TZ_INFO "UTC2"

TwoWire I2CAM2320 = TwoWire(0);
Adafruit_AM2320 am2320 = Adafruit_AM2320(&I2CAM2320);

String setupLog;

const int CHARGING_PIN = GPIO_NUM_5;
const int LOW_BAT_PIN = GPIO_NUM_6;

const int BAT_PG_PIN = GPIO_NUM_40;
const int BAT_LBO_PIN = GPIO_NUM_41;
const int BAT_STAT2_PIN = GPIO_NUM_42;

const int LOW_BATTERY_VOLTAGE = 4100;

// Declare InfluxDB client instance with preconfigured InfluxCloud certificate
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);

// Declare Data point
Point sensor("air_quality");

String h2S(uint8_t num)
{
	return (num < 16 ? "0" : "") + String(num, HEX);
}

void setup()
{
	Serial.begin(115200);
	pinMode(CHARGING_PIN, OUTPUT); // Indicator LED
	pinMode(LOW_BAT_PIN, OUTPUT);	 // Indicator LED
	pinMode(BAT_PG_PIN, INPUT_PULLUP);
	pinMode(BAT_LBO_PIN, INPUT_PULLUP);
	pinMode(BAT_STAT2_PIN, INPUT_PULLUP);

	delay(10);

	if (!I2CAM2320.begin(21, 33))
	{
		setupLog += "Couldn't initialize I2C\n";
		setupLog += String("Error:") + esp_err_to_name(I2CAM2320.last_err);
	}
	else
	{
		// unsigned status = bme.begin(0x77, &I2CAM2320);
		bool status = am2320.begin();

		if (!status)
		{
			setupLog += "Could not find a valid AM320 sensor, check wiring\n";
			setupLog += "\n";
		}
	}

	// Setup wifi
	WiFi.mode(WIFI_STA);
	wifiMulti.addAP(ssid, password);

	Serial.print("Connecting to wifi");
	while (wifiMulti.run() != WL_CONNECTED)
	{
		Serial.print(".");
		delay(100);
	}
	Serial.println();

	// Accurate time is necessary for certificate validation and writing in batches
	// We use the NTP servers in your area as provided by: https://www.pool.ntp.org/zone/
	// Syncing progress and the time will be printed to Serial.
	timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

	// Check server connection
	if (client.validateConnection())
	{
		Serial.print("Connected to InfluxDB: ");
		Serial.println(client.getServerUrl());
	}
	else
	{
		Serial.print("InfluxDB connection failed: ");
		Serial.println(client.getLastErrorMessage());
	}

	uint8_t macAddress[10];
	if (esp_efuse_mac_get_default(macAddress) == ESP_OK)
	{

		String myID = "";
		for (int i = 5; i >= 0; i--)
			myID += h2S(macAddress[i]);
		sensor.addTag("SensorID", myID);
	}
	else
	{
		Serial.print("Couldn't read MAC address");
		sensor.addTag("SensorID", "UNKNOWN");
	}
	// Add tags to the data point
	sensor.addTag("device", DEVICE);
	sensor.addTag("SSID", WiFi.SSID());

	adc1_config_width(ADC_WIDTH_BIT_13);
	adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);
}

int batteryPoweredDelay = 10000; // 10 second delay
int millisecondsBetweenPosts = 600000;
int accumDelayBeforeSending = 0;

void loop()
{
	// Clear fields for reusing the point. Tags will remain the same as set above.
	sensor.clearFields();

	// Store measured value into point
	float temp = am2320.readTemperature();
	delay(200);
	float humidity = am2320.readHumidity();
	Serial.print("Temp: ");
	Serial.print(temp);
	Serial.print(" C  Humidity:");
	Serial.println(humidity);

	sensor.addField("Temp", temp);
	sensor.addField("RH", humidity);

	bool pg = digitalRead(BAT_PG_PIN) == LOW;
	bool lbo = digitalRead(BAT_LBO_PIN) == LOW;
	bool stat2 = digitalRead(BAT_STAT2_PIN) == LOW;
	Serial.println("BAT_PG_PIN: " + String(digitalRead(BAT_PG_PIN)));
	Serial.println("BAT_LBO_PIN: " + String(digitalRead(BAT_LBO_PIN)));
	Serial.println("BAT_STAT2_PIN: " + String(digitalRead(BAT_STAT2_PIN)));

	bool charging = pg && lbo && !stat2;
	bool charge_complete = pg && !lbo && stat2;
	bool low_battery = !pg && lbo && !stat2;

	sensor.addField("BAT_PG", pg);
	sensor.addField("BAT_LBO", lbo);
	sensor.addField("BAT_STAT2", stat2);
	sensor.addField("BAT_CHARGING", charging);
	sensor.addField("BAT_CHARGE_COMPLETE", charge_complete);
	sensor.addField("BAT_LOW", low_battery);

	// Check WiFi connection and reconnect if needed
	if (wifiMulti.run() != WL_CONNECTED)
	{
		Serial.println("Wifi connection lost");
	}

	digitalWrite(CHARGING_PIN, charging || charge_complete);
	digitalWrite(LOW_BAT_PIN, low_battery || charge_complete);
	if (pg)
	{
		// We are connected to power supply, we don't need to go to deep sleep
		// Write point
		accumDelayBeforeSending += batteryPoweredDelay;
		if (accumDelayBeforeSending > millisecondsBetweenPosts)
		{
			// Print what are we exactly writing
			Serial.print("Writing: ");
			Serial.println(sensor.toLineProtocol());
			if (!client.writePoint(sensor))
			{
				Serial.print("InfluxDB write failed: ");
				Serial.println(client.getLastErrorMessage());
			}
			accumDelayBeforeSending = 0;
		}
		delay(batteryPoweredDelay);
	}	else {
		// Write point
		client.writePoint(sensor);
		// Running on battery, deep sleep is needed.
		esp_sleep_enable_timer_wakeup(600 * 1000000ULL); // 10 minutes
		esp_deep_sleep_start();
	}
}
