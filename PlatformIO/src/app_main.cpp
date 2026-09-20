// Arduino entry points (setup/loop). Named app_main.cpp rather than main.cpp so
// its object never collides with the Arduino core's own main.cpp.o in the
// arduino+espidf build, where core objects land in the build root.
#include <Arduino.h>

#include "defs.h"
#include "tasks.h"

void setup()
{
#if enableDebug
  // UART0's TX buffer defaults to 0, so every debug printf blocks until its
  // bytes have left the wire at 115200 baud — logging from the bridge task
  // then stalls USB traffic. A 4 KB ring absorbs bursts without blocking.
  Serial.setTxBufferSize(4096);
  Serial.begin(serialDebugBaud);
  delay(200);
#endif
  startTasks();
}

void loop()
{
  delay(1000);
}
