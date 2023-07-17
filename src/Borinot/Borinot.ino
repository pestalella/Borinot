#include "passwords.h"

#include "Adafruit_Sensor.h"
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

const int CHARGING_PIN = GPIO_NUM_5;
const int LOW_BAT_PIN = GPIO_NUM_6;
const int USB_SENSE = GPIO_NUM_10;

const int BAT_CHARGE_ENABLE = GPIO_NUM_7;

const int BATTERY_VOLTAGE_START_CHARGE = 3500;
const int BATTERY_VOLTAGE_STOP_CHARGE = 4150;

int batteryPoweredDelay = 10000; // 10 second delay
int millisecondsBetweenPosts = 600000;
int accumDelayBeforeSending = 0;

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
	pinMode(CHARGING_PIN, OUTPUT); // Indicator LED

	pinMode(USB_SENSE, INPUT_PULLDOWN); // USB sense pin (connected to USB 5V)
	pinMode(BAT_CHARGE_ENABLE, OUTPUT); // Battery charge enable pin

	delay(10);
	digitalWrite(BAT_CHARGE_ENABLE, 0); // Disable charging when starting

	if (!I2CAM2320.begin(21, 33))
	{
		Serial.println("Couldn't initialize I2C");
	}
	else
	{
		// unsigned status = bme.begin(0x77, &I2CAM2320);
		bool status = am2320.begin();

		if (!status)
		{
			Serial.println("Could not find a valid AM320 sensor, check wiring");
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

	uint32_t analogReading = adc1_get_raw(ADC1_CHANNEL_3);
	uint32_t voltageRead = analogReading * 2500 / 8191 * 3730 / 3554;
	uint32_t batVoltage = voltageRead * 2;
	Serial.print("Raw value: " + String(analogReading));
	Serial.print(" bat voltage: ");
	Serial.print(batVoltage);
	Serial.println(" mV");
	sensor.addField("BAT_MILLIVOLTS", batVoltage);

	int canEnableCharge = (batVoltage < BATTERY_VOLTAGE_STOP_CHARGE) ? HIGH : LOW;
	int low_battery = (batVoltage <= BATTERY_VOLTAGE_START_CHARGE)? HIGH:LOW;
	int usb_connected = digitalRead(USB_SENSE);
	int charging = (usb_connected == HIGH) && (canEnableCharge == HIGH) ? HIGH:LOW;

	sensor.addField("USB_CONNECTED", usb_connected == HIGH);
	sensor.addField("LOW_BAT_VOLTAGE", low_battery == HIGH);
	sensor.addField("CHARGING", charging == HIGH);

	digitalWrite(CHARGING_PIN, charging);
	digitalWrite(BAT_CHARGE_ENABLE, charging);
	digitalWrite(LOW_BAT_PIN, low_battery);

	// Check WiFi connection and reconnect if needed
	if (wifiMulti.run() != WL_CONNECTED)
	{
		Serial.println("Wifi connection lost");
	}

	if (usb_connected) {
		uint8_t macAddress[10];
		if (esp_efuse_mac_get_default(macAddress) == ESP_OK)
		{

			String myID = "";
			for (int i = 5; i >= 0; i--)
				myID += h2S(macAddress[i]);
			Serial.println("ID:" + myID);
		}

		if (accumDelayBeforeSending >= millisecondsBetweenPosts) {
			// Write point
			Serial.print("Writing: ");
			Serial.println(sensor.toLineProtocol());
			if (!client.writePoint(sensor))
			{
				Serial.print("InfluxDB write failed: ");
				Serial.println(client.getLastErrorMessage());
			}
			accumDelayBeforeSending = 0;
		} else {
			accumDelayBeforeSending += batteryPoweredDelay;
		}
		delay(batteryPoweredDelay);
	} else {
		if (!client.writePoint(sensor))
		{
			Serial.print("InfluxDB write failed: ");
			Serial.println(client.getLastErrorMessage());
		}
		// Running on battery, deep sleep is needed.
		esp_sleep_enable_timer_wakeup(millisecondsBetweenPosts * 1000ULL);
		esp_deep_sleep_start();
	}
}
