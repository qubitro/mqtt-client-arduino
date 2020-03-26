## Qubitro Arduino MQTT Client Library


Client library that implements the MQTT protocol, customized for Qubitro IoT platform. 

[![GitHub version](https://badge.fury.io/gh/qubitro%2Fmqtt-client-arduino.svg)](https://badge.fury.io/gh/qubitro%2Fmqtt-client-arduino)

> Now available on Arduino Library Manager

![Alt Text](https://raw.github.com/qubitro/mqtt-client-arduino/master/screenshot-library-manager.png)


### How to use

#### 1) Get Device ID and Device Token under device settings


![Alt Text](https://raw.github.com/qubitro/mqtt-client-arduino/master/screenshot_qubitro_device_settings.png)

#### 2) Open QubitroExample.ino and paste credentials

```javascript
char deviceID[] = "PASTE_DEVICE_ID_HERE";
char deviceToken[] = "PASTE_DEVICE_TOKEN_HERE";
```

#### 3) Set Device ID & Device TOKEN

```javascript
mqttClient.setDeviceIdToken(deviceID, deviceToken);
```

### Questions ?

[Visit Qubitro documentation](https://docs.qubitro.com) for details. 
