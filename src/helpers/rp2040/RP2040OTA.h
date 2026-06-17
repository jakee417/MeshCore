#pragma once

#include <MeshCore.h>

namespace mesh::rp2040ota {

bool begin(MainBoard& board, const char* id, const char* hostname = nullptr);
bool start(MainBoard& board, const char* id, char reply[]);
void loop();
bool isRunning();

}
