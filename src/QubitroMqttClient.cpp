#include "QubitroMqttClient.h"

// #define CLIENT_DEBUG

#ifndef htons
  #ifdef __ARM__
    #define htons __REV16
  #else
    #define htons(s) ((s<<8) | (s>>8))
  #endif
#endif

#ifdef __AVR__
#define TX_PAYLOAD_BUFFER_SIZE 128
#else
#define TX_PAYLOAD_BUFFER_SIZE 256
#endif

#define CONNECT      1
#define CONNACK      2
#define PUBLISH      3
#define PUBACK       4
#define PUBREC       5
#define PUBREL       6
#define PUBCOMP      7
#define SUBSCRIBE    8
#define SUBACK       9
#define UNSUBSCRIBE 10
#define UNSUBACK    11
#define PINGREQ     12
#define PINGRESP    13
#define DISCONNECT  14

enum {
  CLIENT_RX_STATE_READ_TYPE,
  CLIENT_RX_STATE_READ_REMAINING_LENGTH,
  CLIENT_RX_STATE_READ_VARIABLE_HEADER,
  CLIENT_RX_STATE_READ_PUBLISH_TOPIC_LENGTH,
  CLIENT_RX_STATE_READ_PUBLISH_TOPIC,
  CLIENT_RX_STATE_READ_PUBLISH_PACKET_ID,
  CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD,
  CLIENT_RX_STATE_DISCARD_PUBLISH_PAYLOAD
};

QubitroMqttClient::QubitroMqttClient(Client& client) :
  _client(&client),
  _onMessage(NULL),
  _cleanSession(true),
  _keepAliveInterval(60 * 1000L),
  _connectionTimeout(30 * 1000L),
  _connectError(SUCCESS),
  _connected(false),
  _subscribeQos(0x00),
  _rxState(CLIENT_RX_STATE_READ_TYPE),
  _txBufferIndex(0),
  _txPayloadBuffer(NULL),
  _txPayloadBufferIndex(0),
  _willBuffer(NULL),
  _willBufferIndex(0),
  _willMessageIndex(0),
  _willFlags(0x00)
{
  setTimeout(0);
}

QubitroMqttClient::~QubitroMqttClient()
{
  if (_willBuffer) {
    free(_willBuffer);

    _willBuffer = NULL;
  }

  if (_txPayloadBuffer) {
    free(_txPayloadBuffer);

    _txPayloadBuffer = NULL;
  }
}

void QubitroMqttClient::onMessage(void(*callback)(int))
{
  _onMessage = callback;
}

int QubitroMqttClient::parseMessage()
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    // already had a message, but only partially read, discard the data
    _rxState = CLIENT_RX_STATE_DISCARD_PUBLISH_PAYLOAD;
  }

  poll();

  if (_rxState != CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    // message not received or not ready
    return 0;
  }

  return _rxLength;
}

String QubitroMqttClient::messageTopic() const
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    // message received and ready for reading
    return _rxMessageTopic;
  }

  return "";
}

int QubitroMqttClient::messageDup() const
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    // message received and ready for reading
    return _rxMessageDup;
  }

  return -1;
}

int QubitroMqttClient::messageQoS() const
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    // message received and ready for reading
    return _rxMessageQoS;
  }

  return -1;
}

int QubitroMqttClient::messageRetain() const
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    // message received and ready for reading
    return _rxMessageRetain;
  }

  return -1;
}

int QubitroMqttClient::beginMessage(const char* topic, unsigned long size, bool retain, uint8_t qos, bool dup)
{
  _txMessageTopic = topic;
  _txMessageRetain = retain;
  _txMessageQoS = qos;
  _txMessageDup = dup;
  _txPayloadBufferIndex = 0;
  _txStreamPayload = (size != 0xffffffffL);

  if (_txStreamPayload) {
    if (!publishHeader(size)) {
      stop();

      return 0;
    }
  }

  return 1;
}

int QubitroMqttClient::beginMessage(const String& topic, unsigned long size, bool retain, uint8_t qos, bool dup)
{
  return beginMessage(topic.c_str(), size, retain, qos, dup);
}

int QubitroMqttClient::beginMessage(const char* topic, bool retain, uint8_t qos, bool dup)
{
  return beginMessage(topic, 0xffffffffL, retain, qos, dup);
}

int QubitroMqttClient::beginMessage(const String& topic, bool retain, uint8_t qos, bool dup)
{
  return beginMessage(topic.c_str(), retain, qos, dup);
}

int QubitroMqttClient::endMessage()
{
  if (!_txStreamPayload) {
    if (!publishHeader(_txPayloadBufferIndex) ||
        (clientWrite(_txPayloadBuffer, _txPayloadBufferIndex) != _txPayloadBufferIndex)) {
      stop();

      return 0;
    }
  }

  _txStreamPayload = false;

  if (_txMessageQoS) {
    if (_txMessageQoS == 2) {
      // wait for PUBREC
      _returnCode = -1;

      for (unsigned long start = millis(); ((millis() - start) < _connectionTimeout) && clientConnected();) {
        poll();

        if (_returnCode != -1) {
          if (_returnCode == 0) {
            break;
          } else {
            return 0;
          }
        }
      }

      // reply with PUBREL
      pubrel(_txPacketId);
    }

    // wait for PUBACK or PUBCOMP
    _returnCode = -1;

    for (unsigned long start = millis(); ((millis() - start) < _connectionTimeout) && clientConnected();) {
      poll();

      if (_returnCode != -1) {
        return (_returnCode == 0);
      }
    }

    return 0;
  }

  return 1;
}

int QubitroMqttClient::beginWill(const char* topic, unsigned short size, bool retain, uint8_t qos)
{
  int topicLength = strlen(topic);
  size_t willLength = (2 + topicLength + 2 + size);

  if (qos > 2) {
    // invalid QoS
  }

  _willBuffer = (uint8_t*)realloc(_willBuffer, willLength);

  _txBuffer = _willBuffer;
  _txBufferIndex = 0;
  writeString(topic, topicLength);
  write16(0); // dummy size for now
  _willMessageIndex = _txBufferIndex;

  _willFlags = (qos << 3) | 0x04;
  if (retain) {
    _willFlags |= 0x20;
  }

  return 0;
}

int QubitroMqttClient::beginWill(const String& topic, unsigned short size, bool retain, uint8_t qos)
{
  return beginWill(topic.c_str(), size, retain, qos);
}

int QubitroMqttClient::beginWill(const char* topic, bool retain, uint8_t qos)
{
  return beginWill(topic, TX_PAYLOAD_BUFFER_SIZE, retain, qos);
}

int QubitroMqttClient::beginWill(const String& topic, bool retain, uint8_t qos)
{
  return beginWill(topic.c_str(), retain, qos);
}

int QubitroMqttClient::endWill()
{
  // update the index
  _willBufferIndex = _txBufferIndex;

  // update the will message size
  _txBufferIndex = (_willMessageIndex - 2);
  write16(_willBufferIndex - _willMessageIndex);

  _txBuffer = NULL;
  _willMessageIndex = 0;

  return 1;
}

int QubitroMqttClient::subscribe(const char* topic, uint8_t qos)
{
  int topicLength = strlen(topic);
  int remainingLength = topicLength + 5;

  if (qos > 2) {
    // invalid QoS
    return 0;
  }

  _txPacketId++;

  if (_txPacketId == 0) {
    _txPacketId = 1;
  }

  uint8_t packetBuffer[5 + remainingLength];

  beginPacket(SUBSCRIBE, 0x02, remainingLength, packetBuffer);
  write16(_txPacketId);
  writeString(topic, topicLength);
  write8(qos);

  if (!endPacket()) {
    stop();

    return 0;
  }

  _returnCode = -1;
  _subscribeQos = 0x80;

  for (unsigned long start = millis(); ((millis() - start) < _connectionTimeout) && clientConnected();) {
    poll();

    if (_returnCode != -1) {
      _subscribeQos = _returnCode;

      return (_returnCode >= 0 && _returnCode <= 2);
    }
  }

  stop();

  return 0;
}

int QubitroMqttClient::subscribe(const String& topic, uint8_t qos)
{
  return subscribe(topic.c_str(), qos);
}

int QubitroMqttClient::unsubscribe(const char* topic)
{
  int topicLength = strlen(topic);
  int remainingLength = topicLength + 4;

  _txPacketId++;

  if (_txPacketId == 0) {
    _txPacketId = 1;
  }

  uint8_t packetBuffer[5 + remainingLength];

  beginPacket(UNSUBSCRIBE, 0x02, remainingLength, packetBuffer);
  write16(_txPacketId);
  writeString(topic, topicLength);

  if (!endPacket()) {
    stop();

    return 0;
  }

  _returnCode = -1;

  for (unsigned long start = millis(); ((millis() - start) < _connectionTimeout) && clientConnected();) {
    poll();

    if (_returnCode != -1) {
      return (_returnCode == 0);
    }
  }

  stop();

  return 0;
}

int QubitroMqttClient::unsubscribe(const String& topic)
{
  return unsubscribe(topic.c_str());
}

void QubitroMqttClient::poll()
{
  if (clientAvailable() == 0 && !clientConnected()) {
    _rxState = CLIENT_RX_STATE_READ_TYPE;
    _connected = false;
  }

  while (clientAvailable()) {
    byte b = clientRead();
    _lastRx = millis();

    switch (_rxState) {
      case CLIENT_RX_STATE_READ_TYPE: {
        _rxType = (b >> 4);
        _rxFlags = (b & 0x0f);
        _rxLength = 0;
        _rxLengthMultiplier = 1;

        _rxState = CLIENT_RX_STATE_READ_REMAINING_LENGTH;
        break;
      }

      case CLIENT_RX_STATE_READ_REMAINING_LENGTH: {
        _rxLength += (b & 0x7f) * _rxLengthMultiplier;

        _rxLengthMultiplier *= 128;

        if (_rxLengthMultiplier > (128 * 128 * 128L)) {
          // malformed
          stop();

          return;
        }

        if ((b & 0x80) == 0) { // length done
          bool malformedResponse = false;

          if (_rxType == CONNACK || 
              _rxType == PUBACK  ||
              _rxType == PUBREC  || 
              _rxType == PUBCOMP ||
              _rxType == UNSUBACK) {
            malformedResponse = (_rxFlags != 0x00 || _rxLength != 2);
          } else if (_rxType == PUBLISH) {
            malformedResponse = ((_rxFlags & 0x06) == 0x06);
          } else if (_rxType == PUBREL) {
            malformedResponse = (_rxFlags != 0x02 || _rxLength != 2);
          } else if (_rxType == SUBACK) { 
            malformedResponse = (_rxFlags != 0x00 || _rxLength != 3);
          } else if (_rxType == PINGRESP) {
            malformedResponse = (_rxFlags != 0x00 || _rxLength != 0);
          } else {
            // unexpected type
            malformedResponse = true;
          }

          if (malformedResponse) {
            stop();
            return;
          }

          if (_rxType == PUBLISH) {
            _rxMessageDup = (_rxFlags & 0x80) != 0;
            _rxMessageQoS = (_rxFlags >> 1) & 0x03;
            _rxMessageRetain = (_rxFlags & 0x01);

            _rxState = CLIENT_RX_STATE_READ_PUBLISH_TOPIC_LENGTH;
          } else if (_rxLength == 0) {
            _rxState = CLIENT_RX_STATE_READ_TYPE;
          } else {
            _rxState = CLIENT_RX_STATE_READ_VARIABLE_HEADER;
          }

          _rxMessageIndex = 0;
        }
        break;
      }

      case CLIENT_RX_STATE_READ_VARIABLE_HEADER: {
        _rxMessageBuffer[_rxMessageIndex++] = b;

        if (_rxMessageIndex == _rxLength) {
          _rxState = CLIENT_RX_STATE_READ_TYPE;

          if (_rxType == CONNACK) {
            _returnCode = _rxMessageBuffer[1];
          } else if (_rxType == PUBACK   ||
                      _rxType == PUBREC  ||
                      _rxType == PUBCOMP ||
                      _rxType == UNSUBACK) {
            uint16_t packetId = (_rxMessageBuffer[0] << 8) | _rxMessageBuffer[1];

            if (packetId == _txPacketId) {
              _returnCode = 0;
            }
          } else if (_rxType == PUBREL) {
            uint16_t packetId = (_rxMessageBuffer[0] << 8) | _rxMessageBuffer[1];

            if (_txStreamPayload) {
              // ignore, can't send as in the middle of a publish
            } else {
              pubcomp(packetId);
            }
          } else if (_rxType == SUBACK) {
            uint16_t packetId = (_rxMessageBuffer[0] << 8) | _rxMessageBuffer[1];

            if (packetId == _txPacketId) {
              _returnCode = _rxMessageBuffer[2];
            }
          }
        }
        break;
      }

      case CLIENT_RX_STATE_READ_PUBLISH_TOPIC_LENGTH: {
        _rxMessageBuffer[_rxMessageIndex++] = b;

        if (_rxMessageIndex == 2) {
          _rxMessageTopicLength = (_rxMessageBuffer[0] << 8) | _rxMessageBuffer[1];
          _rxLength -= 2;
          
          _rxMessageTopic = "";
          _rxMessageTopic.reserve(_rxMessageTopicLength);

          if (_rxMessageQoS) {
            if (_rxLength < (_rxMessageTopicLength + 2)) {
              stop();
              return;
            }
          } else {
            if (_rxLength < _rxMessageTopicLength) {
              stop();
              return;
            }
          }

          _rxMessageIndex = 0;
          _rxState = CLIENT_RX_STATE_READ_PUBLISH_TOPIC;
        }

        break;
      }

      case CLIENT_RX_STATE_READ_PUBLISH_TOPIC: {
        _rxMessageTopic += (char)b;

        if (_rxMessageTopicLength == _rxMessageTopic.length()) {
          _rxLength -= _rxMessageTopicLength;

          if (_rxMessageQoS) {
            _rxState = CLIENT_RX_STATE_READ_PUBLISH_PACKET_ID;
          } else {
            _rxState = CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD;

            if (_onMessage) {
              _onMessage(_rxLength);

              if (_rxLength == 0) {
                _rxState = CLIENT_RX_STATE_READ_TYPE;
              }
            }
          }
        }

        break;
      }

      case CLIENT_RX_STATE_READ_PUBLISH_PACKET_ID: {
        _rxMessageBuffer[_rxMessageIndex++] = b;

        if (_rxMessageIndex == 2) {
          _rxLength -= 2;

          _rxPacketId = (_rxMessageBuffer[0] << 8) | _rxMessageBuffer[1];

          _rxState = CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD;

          if (_onMessage) {
            _onMessage(_rxLength);
          }

          if (_rxLength == 0) {
            // no payload to read, ack zero length message
            ackRxMessage();

            if (_onMessage) {
              _rxState = CLIENT_RX_STATE_READ_TYPE;
            }
          }
        }

        break;
      }

      case CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD:
      case CLIENT_RX_STATE_DISCARD_PUBLISH_PAYLOAD: {
        if (_rxLength > 0) {
          _rxLength--;
        }

        if (_rxLength == 0) {
          _rxState = CLIENT_RX_STATE_READ_TYPE;
        } else {
          _rxState = CLIENT_RX_STATE_DISCARD_PUBLISH_PAYLOAD;
        }

        break;
      }
    }

    if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
      break;
    }
  }

  if (_connected) {
    unsigned long now = millis();

    if ((now - _lastPingTx) >= _keepAliveInterval) {
      _lastPingTx = now;

      ping();
    } else if ((now - _lastRx) >= (_keepAliveInterval * 2)) {
      stop();
    }
  }
}

int QubitroMqttClient::connect(IPAddress ip, uint16_t port)
{
  return connect(ip, NULL, port);
}

int QubitroMqttClient::connect(const char *host, uint16_t port)
{
  return connect((uint32_t)0, host, port);
}

size_t QubitroMqttClient::write(uint8_t b)
{
  return write(&b, sizeof(b));
}

size_t QubitroMqttClient::write(const uint8_t *buf, size_t size)
{
  if (_willMessageIndex) {
    return writeData(buf, size);
  }

  if (_txStreamPayload) {
    return clientWrite(buf, size);
  }

  if ((_txPayloadBufferIndex + size) >= TX_PAYLOAD_BUFFER_SIZE) {
    size = (TX_PAYLOAD_BUFFER_SIZE - _txPayloadBufferIndex);
  }

  if (_txPayloadBuffer == NULL) {
    _txPayloadBuffer = (uint8_t*)malloc(TX_PAYLOAD_BUFFER_SIZE);
  }

  memcpy(&_txPayloadBuffer[_txPayloadBufferIndex], buf, size);
  _txPayloadBufferIndex += size;

  return size;
}

int QubitroMqttClient::available()
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    return _rxLength;
  }

  return 0;
}

int QubitroMqttClient::read()
{
  byte b;

  if (read(&b, sizeof(b)) != sizeof(b)) {
    return -1;
  }

  return b;
}

int QubitroMqttClient::read(uint8_t *buf, size_t size)
{
  size_t result = 0;

  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    size_t avail = available();

    if (size > avail) {
      size = avail;
    }

    while (result < size) {
      int b = clientTimedRead();

      if (b == -1) {
        break;
      } 

      result++;
      *buf++ = b;
    }

    if (result > 0) {
      _rxLength -= result;

      if (_rxLength == 0) {
        ackRxMessage();

        _rxState = CLIENT_RX_STATE_READ_TYPE;
      }
    }
  }

  return result;
}

int QubitroMqttClient::peek()
{
  if (_rxState == CLIENT_RX_STATE_READ_PUBLISH_PAYLOAD) {
    return clientPeek();
  }

  return -1;
}

void QubitroMqttClient::flush()
{
}

void QubitroMqttClient::stop()
{
  if (connected()) {
    disconnect();
  }

  _connected = false;
  _client->stop();
}

uint8_t QubitroMqttClient::connected()
{
  return clientConnected() && _connected;
}

QubitroMqttClient::operator bool()
{
  return true;
}

void QubitroMqttClient::setId(const char* id)
{
  _id = id;
}

void QubitroMqttClient::setId(const String& id)
{
  _id = id;
}

void QubitroMqttClient::setDeviceIdToken(const char* deviceId, const char* token)
{
  _deviceId = deviceId;
  _token = token;
}

void QubitroMqttClient::setDeviceIdToken(const String& deviceId, const String& token)
{
  _deviceId = deviceId;
  _token = token;
}

void QubitroMqttClient::setCleanSession(bool cleanSession)
{
  _cleanSession = cleanSession;
}

void QubitroMqttClient::setKeepAliveInterval(unsigned long interval)
{
  _keepAliveInterval = interval;
}

void QubitroMqttClient::setConnectionTimeout(unsigned long timeout)
{
  _connectionTimeout = timeout;
}

int QubitroMqttClient::connectError() const
{
  return _connectError;
}

int QubitroMqttClient::subscribeQoS() const
{
  return _subscribeQos;
}

int QubitroMqttClient::connect(IPAddress ip, const char* host, uint16_t port)
{
  if (clientConnected()) {
    _client->stop();
  }
  _rxState = CLIENT_RX_STATE_READ_TYPE;
  _connected = false;
  _txPacketId = 0x0000;

  if (host) {
    if (!_client->connect(host, port)) {
      _connectError = CONNECTION_REFUSED;
      return 0;
    }
  } else {
    if (!_client->connect(ip, port)) {
      _connectError = CONNECTION_REFUSED;
      return 0;
    }
  }

  _lastRx = millis();

  String id = _id;
  int idLength = id.length();
  int deviceIdLength = _deviceId.length();
  int tokenLength = _token.length();
  uint8_t flags = 0;

  if (idLength == 0) {
    char tempId[17];

    snprintf(tempId, sizeof(tempId), "Arduino-%.8lx", millis());

    id = tempId;
    idLength = sizeof(tempId) - 1;
  }

  struct __attribute__ ((packed)) {
    struct {
      uint16_t length;
      char value[4];
    } protocolName;
    uint8_t level;
    uint8_t flags;
    uint16_t keepAlive;
  } connectVariableHeader;

  size_t remainingLength = sizeof(connectVariableHeader) + (2 + idLength) + _willBufferIndex;

  if (deviceIdLength) {
    flags |= 0x80;

    remainingLength += (2 + deviceIdLength);
  }

  if (tokenLength) {
    flags |= 0x40;

    remainingLength += (2 + tokenLength);
  }

  flags |= _willFlags;

  if (_cleanSession) {
    flags |= 0x02; // clean session
  }

  connectVariableHeader.protocolName.length = htons(sizeof(connectVariableHeader.protocolName.value));
  memcpy(connectVariableHeader.protocolName.value, "MQTT", sizeof(connectVariableHeader.protocolName.value));
  connectVariableHeader.level = 0x04;
  connectVariableHeader.flags = flags;
  connectVariableHeader.keepAlive = htons(_keepAliveInterval / 1000);

  uint8_t packetBuffer[5 + remainingLength];

  beginPacket(CONNECT, 0x00, remainingLength, packetBuffer);
  writeData(&connectVariableHeader, sizeof(connectVariableHeader));
  writeString(id.c_str(), idLength);

  if (_willBufferIndex) {
    writeData(_willBuffer, _willBufferIndex);
  }

  if (deviceIdLength) {
    writeString(_deviceId.c_str(), deviceIdLength);
  }

  if (tokenLength) {
    writeString(_token.c_str(), tokenLength);
  }

  if (!endPacket()) {
    _client->stop();

    _connectError = SERVER_UNAVAILABLE;

    return 0;
  }

  _returnCode = CONNECTION_TIMEOUT;

  for (unsigned long start = millis(); ((millis() - start) < _connectionTimeout) && clientConnected();) {
    poll();

    if (_returnCode != CONNECTION_TIMEOUT) {
      break;
    }
  }

  _connectError = _returnCode;

  if (_returnCode == SUCCESS) {
    _connected = true;

    return 1;
  }

  _client->stop();

  return 0;
}

int QubitroMqttClient::publishHeader(size_t length)
{
  int topicLength = _txMessageTopic.length();
  int headerLength = topicLength + 2;

  if (_txMessageQoS > 2) {
    // invalid QoS
    return 0;
  }

  if (_txMessageQoS) {
    // add two for packet id
    headerLength += 2;

    _txPacketId++;

    if (_txPacketId == 0) {
      _txPacketId = 1;
    }
  }

  // only for packet header
  uint8_t packetHeaderBuffer[5 + headerLength];

  uint8_t flags = 0;

  if (_txMessageRetain) {
    flags |= 0x01;
  }

  if (_txMessageQoS) {
    flags |= (_txMessageQoS << 1);
  }

  if (_txMessageDup) {
    flags |= 0x08;
  }

  beginPacket(PUBLISH, flags, headerLength + length, packetHeaderBuffer);
  writeString(_txMessageTopic.c_str(), topicLength);
  if (_txMessageQoS) {
    write16(_txPacketId);
  }

  // send packet header
  return endPacket();
}

void QubitroMqttClient::puback(uint16_t id)
{
  uint8_t packetBuffer[4];

  beginPacket(PUBACK, 0x00, 2, packetBuffer);
  write16(id);
  endPacket();
}

void QubitroMqttClient::pubrec(uint16_t id)
{
  uint8_t packetBuffer[4];

  beginPacket(PUBREC, 0x00, 2, packetBuffer);
  write16(id);
  endPacket();
}

void QubitroMqttClient::pubrel(uint16_t id)
{
  uint8_t packetBuffer[4];

  beginPacket(PUBREL, 0x02, 2, packetBuffer);
  write16(id);
  endPacket();
}

void QubitroMqttClient::pubcomp(uint16_t id)
{
  uint8_t packetBuffer[4];

  beginPacket(PUBCOMP, 0x00, 2, packetBuffer);
  write16(id);
  endPacket();
}

void QubitroMqttClient::ping()
{
  uint8_t packetBuffer[2];

  beginPacket(PINGREQ, 0, 0, packetBuffer);
  endPacket();
}

void QubitroMqttClient::disconnect()
{
  uint8_t packetBuffer[2];

  beginPacket(DISCONNECT, 0, 0, packetBuffer);
  endPacket();
}

int QubitroMqttClient::beginPacket(uint8_t type, uint8_t flags, size_t length, uint8_t* buffer)
{
  _txBuffer = buffer;
  _txBufferIndex = 0;

  write8((type << 4) | flags);

  do {
    uint8_t b = length % 128;
    length /= 128;

    if(length > 0) {
      b |= 0x80;
    }

    _txBuffer[_txBufferIndex++] = b;
  } while (length > 0);

  return _txBufferIndex;
}

int QubitroMqttClient::writeString(const char* s, uint16_t length)
{
  int result = 0;

  result += write16(length);
  result += writeData(s, length);

  return result;
}

int QubitroMqttClient::write8(uint8_t val)
{
  return writeData(&val, sizeof(val));
}

int QubitroMqttClient::write16(uint16_t val)
{
  val = htons(val);

  return writeData(&val, sizeof(val));
}

int QubitroMqttClient::writeData(const void* data, size_t length)
{
  memcpy(&_txBuffer[_txBufferIndex], data, length);
  _txBufferIndex += length;

  return length;
}

int QubitroMqttClient::endPacket()
{
  int result = (clientWrite(_txBuffer, _txBufferIndex) == _txBufferIndex);

  _txBufferIndex = 0;

  return result;
}

void QubitroMqttClient::ackRxMessage()
{
  if (_rxMessageQoS == 1) {
    puback(_rxPacketId);
  } else if (_rxMessageQoS == 2) {
    pubrec(_rxPacketId);
  }
}

int QubitroMqttClient::clientRead()
{
  int result = _client->read();

#ifdef CLIENT_DEBUG
  if (result != -1) {
    Serial.print("RX: ");

    if (result < 16) {
      Serial.print('0');
    }

    Serial.println(result, HEX);
  }
#endif

  return result;
}

uint8_t QubitroMqttClient::clientConnected()
{
  return _client->connected();
}

int QubitroMqttClient::clientAvailable()
{
  return _client->available();
}

int QubitroMqttClient::clientTimedRead()
{
  unsigned long startMillis = millis();

  do {
    if (clientAvailable()) {
      return clientRead();
    } else if (!clientConnected()) {
      return -1;
    }

    yield();
  } while((millis() - startMillis) < 1000);

  return -1;
}

int QubitroMqttClient::clientPeek()
{
  return _client->peek();
}

size_t QubitroMqttClient::clientWrite(const uint8_t *buf, size_t size)
{
#ifdef CLIENT_DEBUG
  Serial.print("TX[");
  Serial.print(size);
  Serial.print("]: ");
  for (size_t i = 0; i < size; i++) {
    uint8_t b = buf[i];

    if (b < 16) {
      Serial.print('0');
    }

    Serial.print(b, HEX);
    Serial.print(' ');
  }
  Serial.println();
#endif

  return _client->write(buf, size);
}
