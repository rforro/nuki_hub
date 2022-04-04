#pragma once

#include <Preferences.h>
#include <WebServer.h>
#include "NukiWrapper.h"
#include "Network.h"

enum class TokenType
{
    None,
    MqttServer,
    MqttPort,
    MqttUser,
    MqttPass,
    MqttPath,
    QueryIntervalLockstate,
    QueryIntervalBattery,
};

class WebCfgServer
{
public:
    WebCfgServer(NukiWrapper* nuki, Network* network, Preferences* preferences);
    ~WebCfgServer() = default;

    void initialize();
    void update();


private:
    void processArgs();
    void serveHtml(String& response);
    void printInputField(String& response, const char* token, const char* description, const char* value, size_t maxLength);
    void printInputField(String& response, const char* token, const char* description, const int value, size_t maxLength);

    void printParameter(String& response, const char* description, const char* value);

    WebServer server;
    NukiWrapper* _nuki;
    Network* _network;
    Preferences* _preferences;

    bool _enabled = true;
};