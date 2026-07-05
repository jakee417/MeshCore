#pragma once

#include <MeshCore.h>

namespace mesh::rp2040ota {

bool begin(MainBoard& board, const char* id, const char* hostname = nullptr, const char* public_key = nullptr, const char* contact_type = nullptr);
bool start(MainBoard& board, const char* id, char reply[]);
void loop();
bool isRunning();

}
