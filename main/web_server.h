#pragma once

#include <WebServer.h>

#include "animation.h"

// Shared HTTP server instance.
extern WebServer server;

// Configure Wi-Fi access point and register all HTTP routes.
void setupWebServer();

