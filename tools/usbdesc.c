/* usbdesc — dump USB configuration descriptors of attached devices (default: all Casio, VID 0x07CF).
 * Usage: usbdesc [vid [pid]]   e.g. usbdesc 0x07CF 0x6802
 * Useful for adding new keyboards; see CONTRIBUTING.md. Part of casio-2000s-midi-bridge. MIT license. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <stdio.h>
#include <stdlib.h>

static void dump(io_service_t svc) {
    IOCFPlugInInterface **plug; SInt32 score; IOUSBDeviceInterface **dev;
    if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score)) { puts("  (plugin failed)"); return; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev); (*plug)->Release(plug);
    UInt16 vid = 0, pid = 0, bcd = 0; UInt8 cls = 0, sub = 0, proto = 0, ncfg = 0;
    (*dev)->GetDeviceVendor(dev, &vid); (*dev)->GetDeviceProduct(dev, &pid); (*dev)->GetDeviceReleaseNumber(dev, &bcd);
    (*dev)->GetDeviceClass(dev, &cls); (*dev)->GetDeviceSubClass(dev, &sub); (*dev)->GetDeviceProtocol(dev, &proto); (*dev)->GetNumberOfConfigurations(dev, &ncfg);
    printf("Device %04X:%04X release=%04X class=0x%02X sub=0x%02X proto=0x%02X configurations=%d\n", vid, pid, bcd, cls, sub, proto, ncfg);
    for (UInt8 c = 0; c < ncfg; c++) {
        IOUSBConfigurationDescriptorPtr cd; if ((*dev)->GetConfigurationDescriptorPtr(dev, c, &cd)) { puts("  (descriptor read failed)"); continue; }
        int len = USBToHostWord(cd->wTotalLength); unsigned char *p = (unsigned char *)cd; int off = 0;
        printf(" Configuration %d: value=%d interfaces=%d attributes=0x%02X maxPower=%dmA\n", c, cd->bConfigurationValue, cd->bNumInterfaces, cd->bmAttributes, cd->MaxPower * 2);
        while (off + 2 <= len) { int l = p[off], t = p[off + 1]; if (l < 2) break;
            printf("  ");
            if (t == 4) printf("INTERFACE   num=%d alt=%d endpoints=%d class=0x%02X sub=0x%02X proto=0x%02X", p[off+2], p[off+3], p[off+4], p[off+5], p[off+6], p[off+7]);
            else if (t == 5) { const char *tt[] = {"control","isoc","bulk","interrupt"}; printf("ENDPOINT    addr=0x%02X %s %s maxPacket=%d interval=%d", p[off+2], (p[off+2] & 0x80) ? "IN " : "OUT", tt[p[off+3] & 3], p[off+4] | (p[off+5] << 8), p[off+6]); }
            else if (t == 0x24) printf("CS_INTERFACE subtype=0x%02X", p[off+2]);
            else if (t == 0x25) printf("CS_ENDPOINT  subtype=0x%02X", p[off+2]);
            else if (t == 2) printf("CONFIG");
            else printf("type=0x%02X", t);
            printf("   [");  for (int i = 0; i < l; i++) printf("%s%02X", i ? " " : "", p[off + i]); printf("]\n");
            off += l; }
    }
    (*dev)->Release(dev);
}

int main(int argc, char **argv) {
    long vid = argc > 1 ? strtol(argv[1], NULL, 0) : 0x07CF, pid = argc > 2 ? strtol(argv[2], NULL, 0) : -1;
    /* IOKit only accepts specific USB key combinations for matching (vendor alone is not one),
     * so match every USB device and filter here. */
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it)) { puts("IOKit matching failed"); return 1; }
    io_service_t svc; int n = 0;
    while ((svc = IOIteratorNext(it))) {
        CFNumberRef v = IORegistryEntryCreateCFProperty(svc, CFSTR(kUSBVendorID), NULL, 0);
        CFNumberRef p = IORegistryEntryCreateCFProperty(svc, CFSTR(kUSBProductID), NULL, 0);
        SInt32 dv = -1, dp = -1;
        if (v) { CFNumberGetValue(v, kCFNumberSInt32Type, &dv); CFRelease(v); }
        if (p) { CFNumberGetValue(p, kCFNumberSInt32Type, &dp); CFRelease(p); }
        if (dv == vid && (pid < 0 || dp == pid)) { dump(svc); n++; }
        IOObjectRelease(svc);
    }
    if (!n) printf("no USB device with vendor 0x%04lX%s found\n", vid, pid >= 0 ? " and that product ID" : "");
    return n ? 0 : 1;
}
