#include "Network.h"
#include "WiFi.h"
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include "Arduino.h"
#include "MqttTopics.h"
#include "PreferencesKeys.h"

Network* nwInst;

Network::Network(Preferences* preferences)
: _mqttClient(_wifiClient),
  _preferences(preferences)
{
    nwInst = this;

    _configTopics.reserve(5);
    _configTopics.push_back(mqtt_topic_config_button_enabled);
    _configTopics.push_back(mqtt_topic_config_led_enabled);
    _configTopics.push_back(mqtt_topic_config_led_brightness);
    _configTopics.push_back(mqtt_topic_config_auto_unlock);
    _configTopics.push_back(mqtt_topic_config_auto_lock);
}

void Network::initialize()
{
    String hostname = _preferences->getString(preference_hostname);
    if(hostname == "")
    {
        hostname = "nukihub";
        _preferences->putString(preference_hostname, hostname);
    }

    WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
    // it is a good practice to make sure your code sets wifi mode how you want it.

    //WiFiManager, Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wm;

    std::vector<const char *> wm_menu;
    wm_menu.push_back("wifi");
    wm_menu.push_back("exit");
    wm.setShowInfoUpdate(false);
    wm.setMenu(wm_menu);
    wm.setHostname(hostname);

    bool res = false;

    if(_cookie.isSet())
    {
        Serial.println(F("Opening WiFi configuration portal."));
        _cookie.clear();
        res = wm.startConfigPortal();
    }
    else
    {
        res = wm.autoConnect(); // password protected ap
    }

    if(!res) {
        Serial.println(F("Failed to connect. Wait for ESP restart."));
        delay(10000);
        ESP.restart();
    }
    else {
        Serial.print(F("WiFi connected."));
        Serial.println(WiFi.localIP().toString());
    }

    Serial.print(F("Host name: "));
    Serial.println(hostname);

    const char* brokerAddr = _preferences->getString(preference_mqtt_broker).c_str();
    strcpy(_mqttBrokerAddr, brokerAddr);

    int port = _preferences->getInt(preference_mqtt_broker_port);
    if(port == 0)
    {
        port = 1883;
        _preferences->putInt(preference_mqtt_broker_port, port);
    }

    String mqttPath = _preferences->getString(preference_mqtt_path);
    if(mqttPath.length() > 0)
    {
        size_t len = mqttPath.length();
        for(int i=0; i < len; i++)
        {
            _mqttPath[i] = mqttPath.charAt(i);
        }
    }
    else
    {
        strcpy(_mqttPath, "nuki");
        _preferences->putString(preference_mqtt_path, _mqttPath);
    }

    String mqttUser = _preferences->getString(preference_mqtt_user);
    if(mqttUser.length() > 0)
    {
        size_t len = mqttUser.length();
        for(int i=0; i < len; i++)
        {
            _mqttUser[i] = mqttUser.charAt(i);
        }
    }

    String mqttPass = _preferences->getString(preference_mqtt_password);
    if(mqttPass.length() > 0)
    {
        size_t len = mqttPass.length();
        for(int i=0; i < len; i++)
        {
            _mqttPass[i] = mqttPass.charAt(i);
        }
    }

    Serial.print(F("MQTT Broker: "));
    Serial.print(_mqttBrokerAddr);
    Serial.print(F(":"));
    Serial.println(port);

    _mqttClient.setServer(_mqttBrokerAddr, port);
    _mqttClient.setCallback(Network::onMqttDataReceivedCallback);
}


bool Network::reconnect()
{
    while (!_mqttClient.connected() && millis() > _nextReconnect)
    {
        Serial.println(F("Attempting MQTT connection"));
        bool success = false;

        if(strlen(_mqttUser) == 0)
        {
            Serial.println(F("MQTT: Connecting without credentials"));
            success = _mqttClient.connect(_preferences->getString(preference_hostname).c_str());
        }
        else
        {
            Serial.print(F("MQTT: Connecting with user: ")); Serial.println(_mqttUser);
            success = _mqttClient.connect(_preferences->getString(preference_hostname).c_str(), _mqttUser, _mqttPass);
        }


        if (success) {
            Serial.println(F("MQTT connected"));
            _mqttConnected = true;
            delay(200);
            subscribe(mqtt_topic_lockstate_action);

            for(auto topic : _configTopics)
            {
                subscribe(topic);
            }
        }
        else
        {
            Serial.print(F("MQTT connect failed, rc="));
            Serial.println(_mqttClient.state());
            _mqttConnected = false;
            _nextReconnect = millis() + 5000;
        }
    }
    return _mqttConnected;
}

void Network::update()
{
    if(!WiFi.isConnected())
    {
        Serial.println(F("WiFi not connected"));
        vTaskDelay( 1000 / portTICK_PERIOD_MS);
    }

    if(!_mqttClient.connected())
    {
        bool success = reconnect();
        if(!success)
        {
            return;
        }
    }

    if(_presenceCsv != nullptr && strlen(_presenceCsv) > 0)
    {
        publishString(mqtt_topic_presence, _presenceCsv);
        _presenceCsv = nullptr;
    }

    _mqttClient.loop();
}

void Network::onMqttDataReceivedCallback(char *topic, byte *payload, unsigned int length)
{
    nwInst->onMqttDataReceived(topic, payload, length);
}

void Network::onMqttDataReceived(char *&topic, byte *&payload, unsigned int &length)
{
    char value[50] = {0};
    size_t l = min(length, sizeof(value)-1);

    for(int i=0; i<l; i++)
    {
        value[i] = payload[i];
    }

    if(comparePrefixedPath(topic, mqtt_topic_lockstate_action))
    {
        if(strcmp(value, "") == 0) return;

        Serial.print(F("Lock action received: "));
        Serial.println(value);
        if(_lockActionReceivedCallback != NULL)
        {
            _lockActionReceivedCallback(value);
        }
        publishString(mqtt_topic_lockstate_action, "");
    }

    for(auto configTopic : _configTopics)
    {
        if(comparePrefixedPath(topic, configTopic))
        {
            if(_configUpdateReceivedCallback != nullptr)
            {
                _configUpdateReceivedCallback(configTopic, value);
            }
        }
    }
}

void Network::publishKeyTurnerState(const Nuki::KeyTurnerState& keyTurnerState, const Nuki::KeyTurnerState& lastKeyTurnerState)
{
    char str[50];

    if(_firstTunerStatePublish || keyTurnerState.lockState != lastKeyTurnerState.lockState)
    {
        memset(&str, 0, sizeof(str));
        lockstateToString(keyTurnerState.lockState, str);
        publishString(mqtt_topic_lockstate_state, str);
    }

    if(_firstTunerStatePublish || keyTurnerState.trigger != lastKeyTurnerState.trigger)
    {
        memset(&str, 0, sizeof(str));
        triggerToString(keyTurnerState.trigger, str);
        publishString(mqtt_topic_lockstate_trigger, str);
    }

    if(_firstTunerStatePublish || keyTurnerState.lastLockActionCompletionStatus != lastKeyTurnerState.lastLockActionCompletionStatus)
    {
        memset(&str, 0, sizeof(str));
        completionStatusToString(keyTurnerState.lastLockActionCompletionStatus, str);
        publishString(mqtt_topic_lockstate_completionStatus, str);
    }

    if(_firstTunerStatePublish || keyTurnerState.doorSensorState != lastKeyTurnerState.doorSensorState)
    {
        memset(&str, 0, sizeof(str));
        doorSensorStateToString(keyTurnerState.doorSensorState, str);
        publishString(mqtt_topic_door_sensor_state, str);
    }

    if(_firstTunerStatePublish || keyTurnerState.criticalBatteryState != lastKeyTurnerState.criticalBatteryState)
    {
        uint8_t level = (keyTurnerState.criticalBatteryState & 0b11111100) >> 1;
        bool critical = (keyTurnerState.criticalBatteryState & 0b00000001) > 0;
        bool charging = (keyTurnerState.criticalBatteryState & 0b00000010) > 0;
        publishInt(mqtt_topic_battery_level, level); // percent
        publishBool(mqtt_topic_battery_critical, critical);
        publishBool(mqtt_topic_battery_charging, charging);
    }

    _firstTunerStatePublish = false;
}

void Network::publishBatteryReport(const Nuki::BatteryReport& batteryReport)
{
    publishFloat(mqtt_topic_battery_voltage, (float)batteryReport.batteryVoltage / 1000.0);
    publishInt(mqtt_topic_battery_drain, batteryReport.batteryDrain); // milliwatt seconds
    publishFloat(mqtt_topic_battery_max_turn_current, (float)batteryReport.maxTurnCurrent / 1000.0);
    publishInt(mqtt_topic_battery_lock_distance, batteryReport.lockDistance); // degrees
}

void Network::publishConfig(const Nuki::Config &config)
{
    publishBool(mqtt_topic_config_button_enabled, config.buttonEnabled == 1);
    publishBool(mqtt_topic_config_led_enabled, config.ledEnabled == 1);
    publishInt(mqtt_topic_config_led_brightness, config.ledBrightness);
}

void Network::publishAdvancedConfig(const Nuki::AdvancedConfig &config)
{
    publishBool(mqtt_topic_config_auto_unlock, config.autoUnLockDisabled == 0);
    publishBool(mqtt_topic_config_auto_lock, config.autoLockEnabled == 1);
}

void Network::publishPresenceDetection(char *csv)
{
    _presenceCsv = csv;
}

void Network::setLockActionReceivedCallback(void (*lockActionReceivedCallback)(const char *))
{
    _lockActionReceivedCallback = lockActionReceivedCallback;
}

void Network::setConfigUpdateReceivedCallback(void (*configUpdateReceivedCallback)(const char *, const char *))
{
    _configUpdateReceivedCallback = configUpdateReceivedCallback;
}

void Network::publishFloat(const char* topic, const float value, const uint8_t precision)
{
    char str[30];
    dtostrf(value, 0, precision, str);
    char path[200] = {0};
    buildMqttPath(topic, path);
    _mqttClient.publish(path, str);
}

void Network::publishInt(const char *topic, const int value)
{

    char str[30];
    itoa(value, str, 10);
    char path[200] = {0};
    buildMqttPath(topic, path);
    _mqttClient.publish(path, str);
}

void Network::publishBool(const char *topic, const bool value)
{
    char str[2] = {0};
    str[0] = value ? '1' : '0';
    char path[200] = {0};
    buildMqttPath(topic, path);
    _mqttClient.publish(path, str);
}

void Network::publishString(const char *topic, const char *value)
{
    char path[200] = {0};
    buildMqttPath(topic, path);
    _mqttClient.publish(path, value);
}


bool Network::isMqttConnected()
{
    return _mqttConnected;
}

void Network::buildMqttPath(const char* path, char* outPath)
{
    int offset = 0;
    for(const char& c : _mqttPath)
    {
        if(c == 0x00)
        {
            break;
        }
        outPath[offset] = c;
        ++offset;
    }
    int i=0;
    while(outPath[i] != 0x00)
    {
        outPath[offset] = path[i];
        ++i;
        ++offset;
    }
    outPath[i+1] = 0x00;
}

void Network::subscribe(const char *path)
{
    char prefixedPath[500];
    buildMqttPath(path, prefixedPath);
    _mqttClient.subscribe(prefixedPath);
}

void Network::restartAndConfigureWifi()
{
    _cookie.set();
    delay(200);
    ESP.restart();
}

bool Network::comparePrefixedPath(const char *fullPath, const char *subPath)
{
    char prefixedPath[500];
    buildMqttPath(subPath, prefixedPath);
    return strcmp(fullPath, prefixedPath) == 0;
}
