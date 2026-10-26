#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum UsbEvent {
  USB_EV_STARTED,
  USB_EV_RESUME,
  USB_EV_SUSPEND,
  USB_EV_STOPPED
};

void usbOnEvent(void (*cb)(UsbEvent));
void usbInit();

int usbHidAvailable();
int usbHidRead();
bool usbHidSendLine(const char *line);
bool usbSystemControl(uint8_t usage);
bool usbSuspended();
bool usbRemoteWakeup();

int usbCdcAvailable();
int usbCdcRead();
bool usbCdcConnected();
void usbCdcWrite(const char *buf, size_t len);
bool usbCdcWriteAll(const char *buf, size_t len);   // blocking, for bulk dumps

#ifdef __cplusplus
}
#endif
