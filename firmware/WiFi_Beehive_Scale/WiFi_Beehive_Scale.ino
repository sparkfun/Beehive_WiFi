/*
 WiFi Beehive Scale: Measures a bee hive weight, humidity, and temp and posts every 60 seconds to internet
 By: Nathan Seidle
 SparkFun Electronics
 Date: November 24th, 2014
 License: This code is public domain but you buy me a beer if you use this and we meet someday (Beerware license).

 You can view the current reported data at https://data.sparkfun.com/streams/wpbq2p0N1rig8WJZRWpO

 OpenScale needs to be configured to 57600 and to trigger on the '!' character.

 This sketch is based on Jim's tutorial here: https://learn.sparkfun.com/tutorials/esp8266-thing-hookup-guide/example-sketch-posting-to-phant

 Power consumption is around 90mA when transmitting and 5mA when in sleep. This allows the system to run for
 around 12 days without any solar charging.

 For this project OpenScale was loaded with firmware that set/forced the calibration factors, 57600bps,
 with the escape character set to ~, trigger to !, and safety RX pin checking turned off.
 This was to avoid any possibility that stray serial could cause OpenScale to get into a funky state. 
 We also use software serial on pins 13 and 5 to allow us to load new firmware without the need to
 unplug OpenScale and send debug statements without fear of causing problems with OpenScale. 
 5 is the status LED and it worked with the LED installed
 but I ultimately removed the SMD LED to save power. Pin 15 had a pull down and didn't seem to work.
 I stayed away from pins 16 and 0 as they are used for system low power and reset. Pins 2 and 14 are used
 for I2C to the humidity sensor.
*/


#define BLYNK_PRINT Serial    // Comment this out to disable prints and save space
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h> //Needed for connection to Blynk App
#include <Phant.h>
#include <TimeLib.h> //Allows us to do year(), hour(), etc. From: https://github.com/PaulStoffregen/Time
#include <WidgetRTC.h> //Installed from the blynk library: https://github.com/blynkkk/blynk-library
#include "Keys.h" //Wifi, Blynk and phant passwords and such
#include <SoftwareSerial.h> //Needed for comm with OpenScale. From: https://github.com/plerup/espsoftwareserial

#include <Wire.h> //Required for on-board humidity sensor (Si7021)
#include "SparkFunHTU21D.h"  //From https://github.com/sparkfun/SparkFun_HTU21D_Breakout_Arduino_Library

#include "DHT.h" //For humidity, from https://github.com/adafruit/DHT-sensor-library
#define DHTPIN 12 //One-wire interface to humidity sensor
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE); //Instatiate the humidity sensor

WidgetRTC rtc; //Instatiate the RTC for Blynk

HTU21D thSense; //Instatiate the on-board humidity sensor

// Pin Definitions
//const int statusLED = 5; //Thing's green onboard status LED
const int batteryPin = A0; //ADC on Thing is 0 to 1V so this 3.7V signal is scaled down 4 to 1 through resistor divider

// Blynk Widget Definitions
const int HIVE_HUMIDITY = V0;
const int HIVE_WEIGHT = V1;
const int HIVE_TEMP = V2;
BLYNK_ATTACH_WIDGET(rtc, V3);
const int LOCAL_HUMIDITY = V4;
const int LOCAL_TEMP = V5;

// Global Variables
float humidity = 0;
float tempF = 0;
float battV;

const unsigned long postRate = 60; //Seconds between posts: The longer between posts the less power we use

SoftwareSerial OpenScale(13, 5, false, 256); //rxPin, txPin, inverse_logic, buffer size

void setup()
{
  Serial.begin(57600);
  
  //pinMode(statusLED, OUTPUT);
  //digitalWrite(statusLED, LOW);

  //readWeight();
  //Serial.println("Done");
  //while(1) delay(10);

  thSense.begin(); // Set up the temperature-humidity sensor

  Blynk.begin(blynkAuthKey, WiFiSSID, WiFiPSK);

  while (Blynk.connect() == false) {
    // Wait until connected
  }

  rtc.begin(); //Begin synchronizing time

  //Force RTC to update
  for (int x = 0 ; x < 10 ; x++)
  {
    Blynk.run();
    delay(10);
  }

  OpenScale.begin(57600);
  
  //connectWiFi(); //Get on wifi
}

void loop()
{
  postToPhant(); //Get readings and post to data.sparkfun.com

  ESP.deepSleep(postRate * 1000L * 1000L); // Sleep for some number of seconds
}


//Take readings and post to phant
int postToPhant()
{
  Serial.println("Getting readings");

  //Check the battery level before we turn on anything else
  int rawBatt = averageAnalogRead(batteryPin);
  int offSet = 40; //Trying to correct to measured values
  battV = 3.3 * (rawBatt - offSet) / 1023.0; //Covert 10-bit ADC (0 to 3.3V ADC is availabe on Blynk Board) to real world voltage
  battV = battV / (1.0 / 2.0); //We have a 10k + 10k resistor divider on the VIN pin to ADC. This scales it to battV.
  Serial.print("battV: ");
  Serial.println(battV);

  // Declare an object from the Phant library - phant
  Phant phant(PhantHost, PublicKey, PrivateKey);

  //Read the humidity sensor in the hive
  humidity = 0.0;
  tempF = 0.0;
  int giveUpCounter = 0;
  while (humidity == 0.0 || tempF == 0.0)
  {
    dht.begin(); //Start humidity sensor
    humidity = dht.readHumidity(); //Read humidity
    tempF = dht.readTemperature(true); //Read temperature as Fahrenheit (isFahrenheit = true)

    if (isnan(humidity) || isnan(tempF))
    {
      humidity = 0.0;
      tempF = 0.0;
    }

    if (giveUpCounter++ > 5) break; //Give up after 5 tries

    delay(1);
  }
  //Serial.print("giveUpCounter: ");
  //Serial.println(giveUpCounter);

  //Try to get a response from OpenScale
  float weightLBS = 0.0;
  for(byte x = 0 ; x < 4 ; x++) //Try 4 times
  {
    weightLBS = readWeight();
    Serial.print("Weight: ");
    Serial.println(weightLBS);
    if(weightLBS > 0.0f || weightLBS < 0.0f) break;
  }

  //Figure out the local time including DST, not UTC
  String localTime = prettyDateTime();
  char tempStr[64];
  localTime.toCharArray(tempStr, 64);

  //Check the onboard humidty and tempt
  float tempCOffset = 0; //-8.33; //From Jim's Blynk Repo https://github.com/sparkfun/Blynk_Board_ESP8266/blob/master/Firmware/BlynkBoard_Core_Firmware/BlynkBoard_BlynkMode.ino
  float localHumidity = thSense.readHumidity(); // Read from humidity sensor
  float localTempC = thSense.readTemperature(); // Read from the temperature sensor
  localTempC += tempCOffset; // Add any offset
  float localTempF = localTempC * 9.0 / 5.0 + 32.0; // Convert to farenheit

  //Update Blynk displays
  Blynk.virtualWrite(HIVE_HUMIDITY, humidity);
  Blynk.virtualWrite(HIVE_WEIGHT, weightLBS);
  Blynk.virtualWrite(HIVE_TEMP, tempF);
  Blynk.virtualWrite(LOCAL_TEMP, localTempF);
  Blynk.virtualWrite(LOCAL_HUMIDITY, localHumidity);

  //Add all this info to the stream
  //  phant.add("tempf", tempF, 2);
  phant.add("tempf", tempF);
  phant.add("humidity", humidity);
  phant.add("battv", battV);
  phant.add("weightlbs", weightLBS);
  phant.add("measurementtime", tempStr);

  // Now connect to data.sparkfun.com, and post our data:
  Serial.println("Posting to phant");

  //digitalWrite(statusLED, LOW); //Turn LED on

  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(PhantHost, httpPort))
  {
    delay(100); //Inserted to avoid blocking
    Blynk.run(); //Let Blynk do its thing

    // If we fail to connect, return 0.
    return 0;
  }
  // If we successfully connected, print our Phant post:
  client.print(phant.post());

  // Read all the lines of the reply from server and print them to Serial
  while (client.available())
  {
    delay(100); //Inserted to avoid blocking
    Blynk.run(); //Let Blynk do its thing

    String line = client.readStringUntil('\r');
    //Serial.print(line); // Trying to avoid using serial
  }

  Serial.println("Post complete.");

  //digitalWrite(statusLED, HIGH); //Turn off LED

  return 1; // Return success
}

//Listen to the serial port for a max of 2 seconds and try to find the weight from OpenScale
//This function assumes the timestamp is on (so it can ignore it)
float readWeight()
{
  int maxListenTime = 750; //Number of miliseconds before giving up
  //Openscale does take some time to power up and take a bunch of readings and average them
  //250ms is too short
  int counter;

  Serial.println("Requesting weight: "); //Debug message

  //Clear out any trash in the buffer
  while (OpenScale.available()) OpenScale.read();

  //Send the trigger character to trigger a read
  OpenScale.println(); //Send a character for OpenScale to get sync'd up
  OpenScale.println();
  OpenScale.print('!');

  //Now we need to spin to the first comma after the time stamp
  counter = 0;
  while (1)
  {
    if(OpenScale.available())
    {
      char incoming = OpenScale.read();
      //Serial.print(incoming); //Debug
      if(incoming == ',') break;
    }
    if (counter++ >= maxListenTime) return (0); //Error
    delay(1);
  }

  Serial.println("OpenScale responded"); //Debug message

  //Now we read the weight
  counter = 0;
  String weightStr;
  while (1)
  {
    if (OpenScale.available())
    {
      char incoming = OpenScale.read();
      if (incoming == ',')
      {
        //We're done!
        return (weightStr.toFloat());
      }
      else
      {
        weightStr.concat(String(incoming));
      }
    }

    if (counter++ == maxListenTime) return (0); //Error
    delay(1);
  }
}

//Takes an average of readings on a given pin
//Returns the average
int averageAnalogRead(byte pinToRead)
{
  byte numberOfReadings = 8;
  unsigned int runningValue = 0;

  for (int x = 0 ; x < numberOfReadings ; x++)
    runningValue += analogRead(pinToRead);
  runningValue /= numberOfReadings;

  return (runningValue);
}

//If we have access to RTC widget we can print the time and date
//This also takes into account DST so set your widget to your normal offset (what it is when DST is not in effect)
String prettyDateTime()
{
  String prettyPrint;

  int localHour = hour(); //hour() is 0 to 23 and is offset by the

  //hour() returns the local hour offset by whatever the user selected in the app on the widget config screen
  //But the offset doesn't take into account DST (roarrrr!). So we do it here.

  if (isDST()) localHour++; //Adjust for DST

  String AMPM = "AM";
  if (localHour > 12)
  {
    localHour -= 12; //Get rid of military time
    AMPM = "PM";
  }
  if (localHour == 12) AMPM = "PM"; //Noon correction
  if (localHour == 0) localHour = 12; //Mid-night correction

  prettyPrint = (String)localHour + ":";

  if (minute() < 10) {
    prettyPrint += "0";
  }
  prettyPrint += (String)minute() + ":";

  if (second() < 10) {
    prettyPrint += "0";
  }
  prettyPrint += (String)second();

  prettyPrint += AMPM;

  return (prettyPrint);
}
//Given UTC month and weekday, figure out if we are in daylight savings or not
//Returns true if we are in DST (USA rule set)
//This code used from https://github.com/nseidle/Daylight_Savings_Time_Example
boolean isDST()
{
  //Since 2007 DST starts on the second Sunday in March and ends the first Sunday of November
  //Let's just assume it's going to be this way for awhile (silly US government!)
  bool dst = false; //Assume we're not in DST

  if (month() > 3 && month() < 11) dst = true; //DST is happening!

  int DoW = weekday(); //Get the day of the week. 0 = Sunday, 6 = Saturday

  //In March, we are DST if our previous Sunday was on or after the 8th.
  int previousSunday = day() - DoW;
  if (month() == 3)
    if (previousSunday >= 8) dst = true;

  //In November we must be before the first Sunday to be dst.
  //That means the previous Sunday must be before the 1st.
  if (month() == 11)
    if (previousSunday <= 0) dst = true;

  return (dst);
}

