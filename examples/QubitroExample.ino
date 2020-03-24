
#include <QubitroMqttClient.h>
#include <WiFiNINA.h> 

WiFiClient wifiClient;
QubitroMqttClient mqttClient(wifiClient);

// WiFi Credentials
char ssid[] = "WiFi_ID";   
char pass[] = "WiFi_PASSWORD";

char deviceID[] = "PASTE_DEVICE_ID_HERE";
char deviceToken[] = "PASTE_DEVICE_TOKEN_HERE";
char host[] = "PASTE_QUBITRO_BROKER_IP";
int port = 1883;

void setup() 
{
  // Initialize serial port
  Serial.begin(9600);
  while (!Serial) {;} 
  
  // connect to Wifi network:
  Serial.println("Connecting to WiFi...");
  
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) 
  {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("Connected to the WiFi");

  // You need to provide device id and device token
   mqttClient.setId(deviceID);
   mqttClient.setDeviceIdToken(deviceID, deviceToken);

  Serial.println("Connecting to Qubitro...");

  if (!mqttClient.connect(host, port)) 
  {
    Serial.println("Connection failed! Error code = ");
    Serial.println(mqttClient.connectError());
    Serial.println("Visit docs.qubitro.com or create a new issue on github.com/qubitro");
    while (1);
  }
  Serial.println("Connected to the Qubitro !");

  mqttClient.onMessage(receivedMessage);    
                                      
  mqttClient.subscribe(deviceID);
}
void loop() 
{
    mqttClient.poll();   
    mqttClient.beginMessage(deviceID);   
    mqttClient.print("{\"Temp\":33}"); //insert key value JSON string object to send
    mqttClient.endMessage();                
    delay(2000);   //wait 2 seconds
}

void receivedMessage(int messageSize) 
{
  Serial.print("New message received:");
  while (mqttClient.available()) 
  {
    Serial.print((char)mqttClient.read());
  }
  Serial.println();
}