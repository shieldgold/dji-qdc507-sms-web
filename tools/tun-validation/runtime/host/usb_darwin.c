// IOKit transport for the selected ADB interface only. No AT, resets,
// alternate-setting changes, pipe-stall clearing, or device seizure.
#include "usb_darwin.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <stdio.h>
#include <stdlib.h>
struct qdc_usb {
  IOUSBDeviceInterface **device;
  IOUSBInterfaceInterface **iface;
  UInt8 in, out;
};
static char last[256];
const char *qdc_error(void) { return last; }
static void error(const char *s, IOReturn r) {
  snprintf(last, sizeof(last), "%s: 0x%08x", s, (unsigned)r);
}
void qdc_close(qdc_usb *h) {
  if (!h)
    return;
  if (h->iface) {
    (*h->iface)->USBInterfaceClose(h->iface);
    (*h->iface)->Release(h->iface);
  }
  if (h->device) {
    (*h->device)->USBDeviceClose(h->device);
    (*h->device)->Release(h->device);
  }
  free(h);
}
qdc_usb *qdc_open(uint16_t vid, uint16_t pid, uint32_t location) {
  io_iterator_t it = 0;
  IOReturn rc = IOServiceGetMatchingServices(
      kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it);
  if (rc != kIOReturnSuccess) {
    error("enumerate", rc);
    return NULL;
  }
  IOUSBDeviceInterface **selected = NULL;
  unsigned matches = 0;
  io_service_t service;
  while ((service = IOIteratorNext(it))) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    rc = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug,
        &score);
    IOObjectRelease(service);
    if (rc != kIOReturnSuccess || !plug)
      continue;
    IOUSBDeviceInterface **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                            (LPVOID *)&dev);
    (*plug)->Release(plug);
    if (!dev)
      continue;
    UInt16 v = 0, p = 0;
    UInt32 l = 0;
    (*dev)->GetDeviceVendor(dev, &v);
    (*dev)->GetDeviceProduct(dev, &p);
    (*dev)->GetLocationID(dev, &l);
    if (v == vid && p == pid && l == location) {
      matches++;
      if (!selected)
        selected = dev;
      else
        (*dev)->Release(dev);
    } else
      (*dev)->Release(dev);
  }
  IOObjectRelease(it);
  if (matches != 1) {
    if (selected)
      (*selected)->Release(selected);
    snprintf(last, sizeof(last),
             "expected one exact VID/PID/location match, got %u", matches);
    return NULL;
  }
  rc = (*selected)->USBDeviceOpen(selected);
  if (rc != kIOReturnSuccess) {
    error("USBDeviceOpen (no seize)", rc);
    (*selected)->Release(selected);
    return NULL;
  }
  qdc_usb *h = calloc(1, sizeof(*h));
  if (!h) {
    (*selected)->USBDeviceClose(selected);
    (*selected)->Release(selected);
    return NULL;
  }
  h->device = selected;
  IOUSBFindInterfaceRequest req = {0xff, 0x42, 1, kIOUSBFindInterfaceDontCare};
  it = 0;
  rc = (*selected)->CreateInterfaceIterator(selected, &req, &it);
  if (rc != kIOReturnSuccess) {
    error("ADB interface iterator", rc);
    qdc_close(h);
    return NULL;
  }
  while ((service = IOIteratorNext(it))) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    rc = IOCreatePlugInInterfaceForService(
        service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug,
        &score);
    IOObjectRelease(service);
    if (rc != kIOReturnSuccess || !plug)
      continue;
    IOUSBInterfaceInterface **iface = NULL;
    (*plug)->QueryInterface(
        plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&iface);
    (*plug)->Release(plug);
    if (!iface)
      continue;
    rc = (*iface)->USBInterfaceOpen(iface);
    if (rc != kIOReturnSuccess) {
      error("ADB USBInterfaceOpen", rc);
      (*iface)->Release(iface);
      continue;
    }
    UInt8 count = 0;
    (*iface)->GetNumEndpoints(iface, &count);
    UInt8 in = 0, out = 0;
    for (UInt8 n = 1; n <= count; n++) {
      UInt8 dir = 0, num = 0, type = 0, interval = 0;
      UInt16 packet = 0;
      if ((*iface)->GetPipeProperties(iface, n, &dir, &num, &type, &packet,
                                      &interval) != kIOReturnSuccess ||
          type != kUSBBulk)
        continue;
      if (dir == kUSBIn)
        in = n;
      else if (dir == kUSBOut)
        out = n;
    }
    if (in && out) {
      h->iface = iface;
      h->in = in;
      h->out = out;
      break;
    }
    (*iface)->USBInterfaceClose(iface);
    (*iface)->Release(iface);
  }
  IOObjectRelease(it);
  if (!h->iface) {
    snprintf(last, sizeof(last),
             "selected device has no claimable ADB bulk interface");
    qdc_close(h);
    return NULL;
  }
  return h;
}
int qdc_write(qdc_usb *h, const void *p, uint32_t n) {
  IOReturn r =
      (*h->iface)->WritePipeTO(h->iface, h->out, (void *)p, n, 2000, 2000);
  if (r != kIOReturnSuccess) {
    error("ADB WritePipeTO", r);
    return -1;
  }
  return 0;
}
int qdc_read(qdc_usb *h, void *p, uint32_t n, uint32_t ms, uint32_t *count) {
  UInt32 size = n;
  IOReturn r = (*h->iface)->ReadPipeTO(h->iface, h->in, p, &size, ms, ms);
  if (r == kIOReturnSuccess) {
    *count = size;
    return 0;
  }
  if (r == kIOReturnTimeout || r == (IOReturn)0xe0004051) {
    *count = 0;
    return 0;
  }
  error("ADB ReadPipeTO", r);
  return -1;
}
