/************************************************************

Copyright 1987, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall not be
used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization from The Open Group.


Copyright 1987 by Digital Equipment Corporation, Maynard, Massachusetts.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Digital not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.

DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

********************************************************/



#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <X11/X.h>
#include "misc.h"
#include "resource.h"
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include "windowstr.h"
#include "inputstr.h"
#include "scrnintstr.h"
#include "cursorstr.h"
#include "dixstruct.h"
#include "ptrveloc.h"
#include "site.h"
#include "xkbsrv.h"
#include "privates.h"
#include "xace.h"
#include "mi.h"

#include "dispatch.h"
#include "swaprep.h"
#include "dixevents.h"
#include "mipointer.h"
#include "eventstr.h"

#include <X11/extensions/XI.h>
#include <X11/extensions/XI2.h>
#include <X11/extensions/XIproto.h>
#include "exglobals.h"
#include "exevents.h"
#include "xiquerydevice.h" /* for SizeDeviceClasses */
#include "xiproperty.h"
#include "enterleave.h" /* for EnterWindow() */
#include "xserver-properties.h"
#include "xichangehierarchy.h" /* For XISendDeviceHierarchyEvent */

/** @file
 * This file handles input device-related stuff.
 */

static int CoreDevicePrivateKeyIndex;
DevPrivateKey CoreDevicePrivateKey = &CoreDevicePrivateKeyIndex;
/* Used to store classes currently not in use by an MD */
static int UnusedClassesPrivateKeyIndex;
DevPrivateKey UnusedClassesPrivateKey = &UnusedClassesPrivateKeyIndex;


static void RecalculateMasterButtons(DeviceIntPtr slave);

/**
 * DIX property handler.
 */
static int
DeviceSetProperty(DeviceIntPtr dev, Atom property, XIPropertyValuePtr prop,
                  BOOL checkonly)
{
    if (property == XIGetKnownProperty(XI_PROP_ENABLED))
    {
        if (prop->format != 8 || prop->type != XA_INTEGER || prop->size != 1)
            return BadValue;

        /* Don't allow disabling of VCP/VCK */
        if ((dev == inputInfo.pointer || dev == inputInfo.keyboard) &&
            !(*(CARD8*)prop->data))
            return BadAccess;

        if (!checkonly)
        {
            if ((*((CARD8*)prop->data)) && !dev->enabled)
                EnableDevice(dev, TRUE);
            else if (!(*((CARD8*)prop->data)) && dev->enabled)
                DisableDevice(dev, TRUE);
        }
    }

    return Success;
}

/* Pair the keyboard to the pointer device. Keyboard events will follow the
 * pointer sprite. Only applicable for master devices.
 * If the client is set, the request to pair comes from some client. In this
 * case, we need to check for access. If the client is NULL, it's from an
 * internal automatic pairing, we must always permit this.
 */
static int
PairDevices(ClientPtr client, DeviceIntPtr ptr, DeviceIntPtr kbd)
{
    if (!ptr)
        return BadDevice;

    /* Don't allow pairing for slave devices */
    if (!IsMaster(ptr) || !IsMaster(kbd))
        return BadDevice;

    if (ptr->spriteInfo->paired)
        return BadDevice;

    if (kbd->spriteInfo->spriteOwner)
    {
        xfree(kbd->spriteInfo->sprite);
        kbd->spriteInfo->sprite = NULL;
        kbd->spriteInfo->spriteOwner = FALSE;
    }

    kbd->spriteInfo->sprite = ptr->spriteInfo->sprite;
    kbd->spriteInfo->paired = ptr;
    ptr->spriteInfo->paired = kbd;
    return Success;
}


/**
 * Find and return the next unpaired MD pointer device.
 */
static DeviceIntPtr
NextFreePointerDevice(void)
{
    DeviceIntPtr dev;
    for (dev = inputInfo.devices; dev; dev = dev->next)
        if (IsMaster(dev) &&
                dev->spriteInfo->spriteOwner &&
                !dev->spriteInfo->paired)
            return dev;
    return NULL;
}

/**
 * Create a new input device and init it to sane values. The device is added
 * to the server's off_devices list.
 *
 * @param deviceProc Callback for device control function (switch dev on/off).
 * @return The newly created device.
 */
DeviceIntPtr
AddInputDevice(ClientPtr client, DeviceProc deviceProc, Bool autoStart)
{
    DeviceIntPtr dev, *prev; /* not a typo */
    DeviceIntPtr devtmp;
    int devid;
    char devind[MAXDEVICES];
    BOOL enabled;

    /* Find next available id, 0 and 1 are reserved */
    memset(devind, 0, sizeof(char)*MAXDEVICES);
    for (devtmp = inputInfo.devices; devtmp; devtmp = devtmp->next)
	devind[devtmp->id]++;
    for (devtmp = inputInfo.off_devices; devtmp; devtmp = devtmp->next)
	devind[devtmp->id]++;
    for (devid = 2; devid < MAXDEVICES && devind[devid]; devid++)
	;

    if (devid >= MAXDEVICES)
	return (DeviceIntPtr)NULL;
    dev =  xcalloc(sizeof(DeviceIntRec) + sizeof(SpriteInfoRec), 1);
    if (!dev)
	return (DeviceIntPtr)NULL;
    dev->id = devid;
    dev->public.processInputProc = (ProcessInputProc)NoopDDA;
    dev->public.realInputProc = (ProcessInputProc)NoopDDA;
    dev->public.enqueueInputProc = EnqueueEvent;
    dev->deviceProc = deviceProc;
    dev->startup = autoStart;

    /* device grab defaults */
    dev->deviceGrab.grabTime = currentTime;
    dev->deviceGrab.ActivateGrab = ActivateKeyboardGrab;
    dev->deviceGrab.DeactivateGrab = DeactivateKeyboardGrab;

    dev->coreEvents = TRUE;

    /* sprite defaults */
    dev->spriteInfo = (SpriteInfoPtr)&dev[1];

    /*  security creation/labeling check
     */
    if (XaceHook(XACE_DEVICE_ACCESS, client, dev, DixCreateAccess)) {
	xfree(dev);
	return NULL;
    }

    inputInfo.numDevices++;

    for (prev = &inputInfo.off_devices; *prev; prev = &(*prev)->next)
        ;
    *prev = dev;
    dev->next = NULL;

    enabled = FALSE;
    XIChangeDeviceProperty(dev, XIGetKnownProperty(XI_PROP_ENABLED),
                           XA_INTEGER, 8, PropModeReplace, 1, &enabled,
                           FALSE);
    XISetDevicePropertyDeletable(dev, XIGetKnownProperty(XI_PROP_ENABLED), FALSE);
    XIRegisterPropertyHandler(dev, DeviceSetProperty, NULL, NULL);

    return dev;
}

void
SendDevicePresenceEvent(int deviceid, int type)
{
    DeviceIntRec dummyDev;
    devicePresenceNotify ev;

    memset(&dummyDev, 0, sizeof(DeviceIntRec));
    ev.type = DevicePresenceNotify;
    ev.time = currentTime.milliseconds;
    ev.devchange = type;
    ev.deviceid = deviceid;
    dummyDev.id = XIAllDevices;
    SendEventToAllWindows(&dummyDev, DevicePresenceNotifyMask,
                          (xEvent*)&ev, 1);
}

/**
 * Enable the device through the driver, add the device to the device list.
 * Switch device ON through the driver and push it onto the global device
 * list. Initialize the DIX sprite or pair the device. All clients are
 * notified about the device being enabled.
 *
 * A master pointer device needs to be enabled before a master keyboard
 * device.
 *
 * @param The device to be enabled.
 * @param sendevent True if an XI2 event should be sent.
 * @return TRUE on success or FALSE otherwise.
 */
Bool
EnableDevice(DeviceIntPtr dev, BOOL sendevent)
{
    DeviceIntPtr *prev;
    int ret;
    DeviceIntPtr other;
    BOOL enabled;
    int flags[MAXDEVICES] = {0};

    for (prev = &inputInfo.off_devices;
	 *prev && (*prev != dev);
	 prev = &(*prev)->next)
	;

    if (!dev->spriteInfo->sprite)
    {
        if (IsMaster(dev))
        {
            /* Sprites appear on first root window, so we can hardcode it */
            if (dev->spriteInfo->spriteOwner)
            {
                InitializeSprite(dev, WindowTable[0]);
                                                 /* mode doesn't matter */
                EnterWindow(dev, WindowTable[0], NotifyAncestor);
            }
            else if ((other = NextFreePointerDevice()) == NULL)
            {
                ErrorF("[dix] cannot find pointer to pair with. "
                       "This is a bug.\n");
                return FALSE;
            } else
                PairDevices(NULL, other, dev);
        } else
        {
            if (dev->coreEvents)
                other = (IsPointerDevice(dev)) ? inputInfo.pointer :
                    inputInfo.keyboard;
            else
                other = NULL; /* auto-float non-core devices */
            AttachDevice(NULL, dev, other);
        }
    }

    /* Before actually enabling the device, we need to make sure the event
     * list's events have enough memory for a ClassesChangedEvent from the
     * device
     */
    if ((*prev != dev) || !dev->inited ||
	((ret = (*dev->deviceProc)(dev, DEVICE_ON)) != Success)) {
        ErrorF("[dix] couldn't enable device %d\n", dev->id);
	return FALSE;
    }
    dev->enabled = TRUE;
    *prev = dev->next;

    for (prev = &inputInfo.devices; *prev; prev = &(*prev)->next)
        ;
    *prev = dev;
    dev->next = NULL;

    enabled = TRUE;
    XIChangeDeviceProperty(dev, XIGetKnownProperty(XI_PROP_ENABLED),
                           XA_INTEGER, 8, PropModeReplace, 1, &enabled,
                           TRUE);

    SendDevicePresenceEvent(dev->id, DeviceEnabled);
    if (sendevent)
    {
        flags[dev->id] |= XIDeviceEnabled;
        XISendDeviceHierarchyEvent(flags);
    }

    RecalculateMasterButtons(dev);

    return TRUE;
}

/**
 * Switch a device off through the driver and push it onto the off_devices
 * list. A device will not send events while disabled. All clients are
 * notified about the device being disabled.
 *
 * Master keyboard devices have to be disabled before master pointer devices
 * otherwise things turn bad.
 *
 * @param sendevent True if an XI2 event should be sent.
 * @return TRUE on success or FALSE otherwise.
 */
Bool
DisableDevice(DeviceIntPtr dev, BOOL sendevent)
{
    DeviceIntPtr *prev, other;
    BOOL enabled;
    int flags[MAXDEVICES] = {0};

    for (prev = &inputInfo.devices;
	 *prev && (*prev != dev);
	 prev = &(*prev)->next)
	;
    if (*prev != dev)
	return FALSE;

    /* float attached devices */
    if (IsMaster(dev))
    {
        for (other = inputInfo.devices; other; other = other->next)
        {
            if (other->u.master == dev)
            {
                AttachDevice(NULL, other, NULL);
                flags[other->id] |= XISlaveDetached;
            }
        }
    }
    else
    {
        for (other = inputInfo.devices; other; other = other->next)
        {
	    if (IsMaster(other) && other->u.lastSlave == dev)
		other->u.lastSlave = NULL;
	}
    }

    if (IsMaster(dev) && dev->spriteInfo->sprite)
    {
        for (other = inputInfo.devices; other; other = other->next)
        {
            if (other->spriteInfo->paired == dev)
            {
                ErrorF("[dix] cannot disable device, still paired. "
                        "This is a bug. \n");
                return FALSE;
            }
        }
    }

    (void)(*dev->deviceProc)(dev, DEVICE_OFF);
    dev->enabled = FALSE;

    /* now that the device is disabled, we can reset the signal handler's
     * last.slave */
    OsBlockSignals();
    for (other = inputInfo.devices; other; other = other->next)
    {
        if (other->last.slave == dev)
            other->last.slave = NULL;
    }
    OsReleaseSignals();

    LeaveWindow(dev);
    SetFocusOut(dev);

    *prev = dev->next;
    dev->next = inputInfo.off_devices;
    inputInfo.off_devices = dev;

    enabled = FALSE;
    XIChangeDeviceProperty(dev, XIGetKnownProperty(XI_PROP_ENABLED),
                           XA_INTEGER, 8, PropModeReplace, 1, &enabled,
                           TRUE);

    SendDevicePresenceEvent(dev->id, DeviceDisabled);
    if (sendevent)
    {
        flags[dev->id] = XIDeviceDisabled;
        XISendDeviceHierarchyEvent(flags);
    }

    RecalculateMasterButtons(dev);

    return TRUE;
}

/**
 * Initialise a new device through the driver and tell all clients about the
 * new device.
 *
 * Must be called before EnableDevice.
 * The device will NOT send events until it is enabled!
 *
 * @param sendevent True if an XI2 event should be sent.
 * @return Success or an error code on failure.
 */
int
ActivateDevice(DeviceIntPtr dev, BOOL sendevent)
{
    int ret = Success;
    ScreenPtr pScreen = screenInfo.screens[0];

    if (!dev || !dev->deviceProc)
        return BadImplementation;

    ret = (*dev->deviceProc) (dev, DEVICE_INIT);
    dev->inited = (ret == Success);
    if (!dev->inited)
        return ret;

    /* Initialize memory for sprites. */
    if (IsMaster(dev) && dev->spriteInfo->spriteOwner)
        pScreen->DeviceCursorInitialize(dev, pScreen);

    SendDevicePresenceEvent(dev->id, DeviceAdded);
    if (sendevent)
    {
        int flags[MAXDEVICES] = {0};
        flags[dev->id] = XISlaveAdded;
        XISendDeviceHierarchyEvent(flags);
    }
    return ret;
}

/**
 * Ring the bell.
 * The actual task of ringing the bell is the job of the DDX.
 */
static void
CoreKeyboardBell(int volume, DeviceIntPtr pDev, pointer arg, int something)
{
    KeybdCtrl *ctrl = arg;

    DDXRingBell(volume, ctrl->bell_pitch, ctrl->bell_duration);
}

static void
CoreKeyboardCtl(DeviceIntPtr pDev, KeybdCtrl *ctrl)
{
    return;
}

/**
 * Device control function for the Virtual Core Keyboard.
 */
int
CoreKeyboardProc(DeviceIntPtr pDev, int what)
{

    switch (what) {
    case DEVICE_INIT:
        if (!InitKeyboardDeviceStruct(pDev, NULL, CoreKeyboardBell,
                                      CoreKeyboardCtl))
        {
            ErrorF("Keyboard initialization failed. This could be a missing "
                   "or incorrect setup of xkeyboard-config.\n");
            return BadValue;
        }
        return Success;

    case DEVICE_ON:
    case DEVICE_OFF:
        return Success;

    case DEVICE_CLOSE:
        return Success;
    }

    return BadMatch;
}

/**
 * Device control function for the Virtual Core Pointer.
 */
int
CorePointerProc(DeviceIntPtr pDev, int what)
{
#define NBUTTONS 10
#define NAXES 2
    BYTE map[NBUTTONS + 1];
    int i = 0;
    Atom btn_labels[NBUTTONS] = {0};
    Atom axes_labels[NAXES] = {0};

    switch (what) {
    case DEVICE_INIT:
        for (i = 1; i <= NBUTTONS; i++)
            map[i] = i;

	btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
	btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
	btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
	btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
	btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
	btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
	btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
	/* don't know about the rest */

	axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
	axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);

        if (!InitPointerDeviceStruct((DevicePtr)pDev, map, NBUTTONS, btn_labels,
                                (PtrCtrlProcPtr)NoopDDA,
                                GetMotionHistorySize(), NAXES, axes_labels))
        {
            ErrorF("Could not initialize device '%s'. Out of memory.\n",
                   pDev->name);
            return BadAlloc; /* IPDS only fails on allocs */
        }
        pDev->valuator->axisVal[0] = screenInfo.screens[0]->width / 2;
        pDev->last.valuators[0] = pDev->valuator->axisVal[0];
        pDev->valuator->axisVal[1] = screenInfo.screens[0]->height / 2;
        pDev->last.valuators[1] = pDev->valuator->axisVal[1];
        break;

    case DEVICE_CLOSE:
        break;

    default:
        break;
    }

    return Success;

#undef NBUTTONS
#undef NAXES
}

/**
 * Initialise the two core devices, VCP and VCK (see events.c).
 * Both devices are not tied to physical devices, but guarantee that there is
 * always a keyboard and a pointer present and keep the protocol semantics.
 *
 * Note that the server MUST have two core devices at all times, even if there
 * is no physical device connected.
 */
void
InitCoreDevices(void)
{
    if (AllocDevicePair(serverClient, "Virtual core",
                        &inputInfo.pointer, &inputInfo.keyboard,
                        CorePointerProc, CoreKeyboardProc,
                        TRUE) != Success)
        FatalError("Failed to allocate core devices");

    if (ActivateDevice(inputInfo.pointer, TRUE) != Success ||
        ActivateDevice(inputInfo.keyboard, TRUE) != Success)
        FatalError("Failed to activate core devices.");
    if (!EnableDevice(inputInfo.pointer, TRUE) ||
        !EnableDevice(inputInfo.keyboard, TRUE))
        FatalError("Failed to enable core devices.");

    InitXTestDevices();
}

/**
 * Activate all switched-off devices and then enable all those devices.
 *
 * Will return an error if no core keyboard or core pointer is present.
 * In theory this should never happen if you call InitCoreDevices() first.
 *
 * InitAndStartDevices needs to be called AFTER the windows are initialized.
 * Devices will start sending events after InitAndStartDevices() has
 * completed.
 *
 * @return Success or error code on failure.
 */
int
InitAndStartDevices(void)
{
    DeviceIntPtr dev, next;

    for (dev = inputInfo.off_devices; dev; dev = dev->next) {
        DebugF("(dix) initialising device %d\n", dev->id);
        if (!dev->inited)
            ActivateDevice(dev, TRUE);
    }

    /* enable real devices */
    for (dev = inputInfo.off_devices; dev; dev = next)
    {
        DebugF("(dix) enabling device %d\n", dev->id);
	next = dev->next;
	if (dev->inited && dev->startup)
	    EnableDevice(dev, TRUE);
    }

    return Success;
}

/**
 * Free the given device class and reset the pointer to NULL.
 */
static void
FreeDeviceClass(int type, pointer *class)
{
    if (!(*class))
        return;

    switch(type)
    {
        case KeyClass:
            {
                KeyClassPtr* k = (KeyClassPtr*)class;
                if ((*k)->xkbInfo)
                {
                    XkbFreeInfo((*k)->xkbInfo);
                    (*k)->xkbInfo = NULL;
                }
                xfree((*k));
                break;
            }
        case ButtonClass:
            {
                ButtonClassPtr *b = (ButtonClassPtr*)class;
                if ((*b)->xkb_acts)
                    xfree((*b)->xkb_acts);
                xfree((*b));
                break;
            }
        case ValuatorClass:
            {
                ValuatorClassPtr *v = (ValuatorClassPtr*)class;

                if ((*v)->motion)
                    xfree((*v)->motion);
                xfree((*v));
                break;
            }
        case FocusClass:
            {
                FocusClassPtr *f = (FocusClassPtr*)class;
                xfree((*f)->trace);
                xfree((*f));
                break;
            }
        case ProximityClass:
            {
                ProximityClassPtr *p = (ProximityClassPtr*)class;
                xfree((*p));
                break;
            }
    }
    *class = NULL;
}

static void
FreeFeedbackClass(int type, pointer *class)
{
    if (!(*class))
        return;

    switch(type)
    {
        case KbdFeedbackClass:
            {
                KbdFeedbackPtr *kbdfeed = (KbdFeedbackPtr*)class;
                KbdFeedbackPtr k, knext;
                for (k = (*kbdfeed); k; k = knext) {
                    knext = k->next;
                    if (k->xkb_sli)
                        XkbFreeSrvLedInfo(k->xkb_sli);
                    xfree(k);
                }
                break;
            }
        case PtrFeedbackClass:
            {
                PtrFeedbackPtr *ptrfeed = (PtrFeedbackPtr*)class;
                PtrFeedbackPtr p, pnext;

                for (p = (*ptrfeed); p; p = pnext) {
                    pnext = p->next;
                    xfree(p);
                }
                break;
            }
        case IntegerFeedbackClass:
            {
                IntegerFeedbackPtr *intfeed = (IntegerFeedbackPtr*)class;
                IntegerFeedbackPtr i, inext;

                for (i = (*intfeed); i; i = inext) {
                    inext = i->next;
                    xfree(i);
                }
                break;
            }
        case StringFeedbackClass:
            {
                StringFeedbackPtr *stringfeed = (StringFeedbackPtr*)class;
                StringFeedbackPtr s, snext;

                for (s = (*stringfeed); s; s = snext) {
                    snext = s->next;
                    xfree(s->ctrl.symbols_supported);
                    xfree(s->ctrl.symbols_displayed);
                    xfree(s);
                }
                break;
            }
        case BellFeedbackClass:
            {
                BellFeedbackPtr *bell = (BellFeedbackPtr*)class;
                BellFeedbackPtr b, bnext;

                for (b = (*bell); b; b = bnext) {
                    bnext = b->next;
                    xfree(b);
                }
                break;
            }
        case LedFeedbackClass:
            {
                LedFeedbackPtr *leds = (LedFeedbackPtr*)class;
                LedFeedbackPtr l, lnext;

                for (l = (*leds); l; l = lnext) {
                    lnext = l->next;
                    if (l->xkb_sli)
                        XkbFreeSrvLedInfo(l->xkb_sli);
                    xfree(l);
                }
                break;
            }
    }
    *class = NULL;
}

static void
FreeAllDeviceClasses(ClassesPtr classes)
{
    if (!classes)
        return;

    FreeDeviceClass(KeyClass, (pointer)&classes->key);
    FreeDeviceClass(ValuatorClass, (pointer)&classes->valuator);
    FreeDeviceClass(ButtonClass, (pointer)&classes->button);
    FreeDeviceClass(FocusClass, (pointer)&classes->focus);
    FreeDeviceClass(ProximityClass, (pointer)&classes->proximity);

    FreeFeedbackClass(KbdFeedbackClass, (pointer)&classes->kbdfeed);
    FreeFeedbackClass(PtrFeedbackClass, (pointer)&classes->ptrfeed);
    FreeFeedbackClass(IntegerFeedbackClass, (pointer)&classes->intfeed);
    FreeFeedbackClass(StringFeedbackClass, (pointer)&classes->stringfeed);
    FreeFeedbackClass(BellFeedbackClass, (pointer)&classes->bell);
    FreeFeedbackClass(LedFeedbackClass, (pointer)&classes->leds);

}

/**
 * Close down a device and free all resources.
 * Once closed down, the driver will probably not expect you that you'll ever
 * enable it again and free associated structs. If you want the device to just
 * be disabled, DisableDevice().
 * Don't call this function directly, use RemoveDevice() instead.
 */
static void
CloseDevice(DeviceIntPtr dev)
{
    ScreenPtr screen = screenInfo.screens[0];
    ClassesPtr classes;
    int j;

    if (!dev)
        return;

    XIDeleteAllDeviceProperties(dev);

    if (dev->inited)
	(void)(*dev->deviceProc)(dev, DEVICE_CLOSE);

    /* free sprite memory */
    if (IsMaster(dev) && dev->spriteInfo->sprite)
        screen->DeviceCursorCleanup(dev, screen);

    /* free acceleration info */
    if(dev->valuator && dev->valuator->accelScheme.AccelCleanupProc)
	dev->valuator->accelScheme.AccelCleanupProc(dev);

    while (dev->xkb_interest)
	XkbRemoveResourceClient((DevicePtr)dev,dev->xkb_interest->resource);

    xfree(dev->name);

    classes = (ClassesPtr)&dev->key;
    FreeAllDeviceClasses(classes);

    if (IsMaster(dev))
    {
        classes = dixLookupPrivate(&dev->devPrivates, UnusedClassesPrivateKey);
        FreeAllDeviceClasses(classes);
    }

    if (DevHasCursor(dev) && dev->spriteInfo->sprite) {
        xfree(dev->spriteInfo->sprite->spriteTrace);
        xfree(dev->spriteInfo->sprite);
    }

    /* a client may have the device set as client pointer */
    for (j = 0; j < currentMaxClients; j++)
    {
        if (clients[j] && clients[j]->clientPtr == dev)
        {
            clients[j]->clientPtr = NULL;
            clients[j]->clientPtr = PickPointer(clients[j]);
        }
    }

    xfree(dev->deviceGrab.sync.event);
    dixFreePrivates(dev->devPrivates);
    xfree(dev);
}

/**
 * Shut down all devices, free all resources, etc.
 * Only useful if you're shutting down the server!
 */
void
CloseDownDevices(void)
{
    DeviceIntPtr dev, next;

    /* Float all SDs before closing them. Note that at this point resources
     * (e.g. cursors) have been freed already, so we can't just call
     * AttachDevice(NULL, dev, NULL). Instead, we have to forcibly set master
     * to NULL and pretend nothing happened.
     */
    for (dev = inputInfo.devices; dev; dev = dev->next)
    {
        if (!IsMaster(dev) && dev->u.master)
            dev->u.master = NULL;
    }

    for (dev = inputInfo.devices; dev; dev = next)
    {
	next = dev->next;
        DeleteInputDeviceRequest(dev);
    }
    for (dev = inputInfo.off_devices; dev; dev = next)
    {
	next = dev->next;
        DeleteInputDeviceRequest(dev);
    }

    CloseDevice(inputInfo.pointer);
    CloseDevice(inputInfo.keyboard);

    inputInfo.devices = NULL;
    inputInfo.off_devices = NULL;
    inputInfo.keyboard = NULL;
    inputInfo.pointer = NULL;
    XkbDeleteRulesDflts();
}

/**
 * Remove the cursor sprite for all devices. This needs to be done before any
 * resources are freed or any device is deleted.
 */
void
UndisplayDevices(void)
{
    DeviceIntPtr dev;
    ScreenPtr screen = screenInfo.screens[0];

    for (dev = inputInfo.devices; dev; dev = dev->next)
        screen->DisplayCursor(dev, screen, NullCursor);
}

/**
 * Remove a device from the device list, closes it and thus frees all
 * resources.
 * Removes both enabled and disabled devices and notifies all devices about
 * the removal of the device.
 *
 * No PresenceNotify is sent for device that the client never saw. This can
 * happen if a malloc fails during the addition of master devices. If
 * dev->init is FALSE it means the client never received a DeviceAdded event,
 * so let's not send a DeviceRemoved event either.
 *
 * @param sendevent True if an XI2 event should be sent.
 */
int
RemoveDevice(DeviceIntPtr dev, BOOL sendevent)
{
    DeviceIntPtr prev,tmp,next;
    int ret = BadMatch;
    ScreenPtr screen = screenInfo.screens[0];
    int deviceid;
    int initialized;
    int flags[MAXDEVICES] = {0};

    DebugF("(dix) removing device %d\n", dev->id);

    if (!dev || dev == inputInfo.keyboard || dev == inputInfo.pointer)
        return BadImplementation;

    initialized = dev->inited;
    deviceid = dev->id;

    if (initialized)
    {
        if (DevHasCursor(dev))
            screen->DisplayCursor(dev, screen, NullCursor);

        DisableDevice(dev, sendevent);
        flags[dev->id] = XIDeviceDisabled;
    }

    prev = NULL;
    for (tmp = inputInfo.devices; tmp; (prev = tmp), (tmp = next)) {
	next = tmp->next;
	if (tmp == dev) {

	    if (prev==NULL)
		inputInfo.devices = next;
	    else
		prev->next = next;

	    flags[tmp->id] = IsMaster(tmp) ? XIMasterRemoved : XISlaveRemoved;
	    CloseDevice(tmp);
	    ret = Success;
	}
    }

    prev = NULL;
    for (tmp = inputInfo.off_devices; tmp; (prev = tmp), (tmp = next)) {
	next = tmp->next;
	if (tmp == dev) {
	    flags[tmp->id] = IsMaster(tmp) ? XIMasterRemoved : XISlaveRemoved;
	    CloseDevice(tmp);

	    if (prev == NULL)
		inputInfo.off_devices = next;
	    else
		prev->next = next;

            ret = Success;
	}
    }

    if (ret == Success && initialized) {
        inputInfo.numDevices--;
        SendDevicePresenceEvent(deviceid, DeviceRemoved);
        if (sendevent)
            XISendDeviceHierarchyEvent(flags);
    }

    return ret;
}

int
NumMotionEvents(void)
{
    /* only called to fill data in initial connection reply.
     * VCP is ok here, it is the only fixed device we have. */
    return inputInfo.pointer->valuator->numMotionEvents;
}

void
RegisterPointerDevice(DeviceIntPtr device)
{
    RegisterOtherDevice(device);
}

void
RegisterKeyboardDevice(DeviceIntPtr device)
{
    RegisterOtherDevice(device);
}

int
dixLookupDevice(DeviceIntPtr *pDev, int id, ClientPtr client, Mask access_mode)
{
    DeviceIntPtr dev;
    int rc;
    *pDev = NULL;

    for (dev=inputInfo.devices; dev; dev=dev->next) {
        if (dev->id == id)
            goto found;
    }
    for (dev=inputInfo.off_devices; dev; dev=dev->next) {
        if (dev->id == id)
	    goto found;
    }
    return BadDevice;

found:
    rc = XaceHook(XACE_DEVICE_ACCESS, client, dev, access_mode);
    if (rc == Success)
	*pDev = dev;
    return rc;
}

void
QueryMinMaxKeyCodes(KeyCode *minCode, KeyCode *maxCode)
{
    if (inputInfo.keyboard) {
	*minCode = inputInfo.keyboard->key->xkbInfo->desc->min_key_code;
	*maxCode = inputInfo.keyboard->key->xkbInfo->desc->max_key_code;
    }
}

/* Notably, this function does not expand the destination's keycode range, or
 * notify clients. */
Bool
SetKeySymsMap(KeySymsPtr dst, KeySymsPtr src)
{
    int i, j;
    KeySym *tmp;
    int rowDif = src->minKeyCode - dst->minKeyCode;

    /* if keysym map size changes, grow map first */
    if (src->mapWidth < dst->mapWidth) {
        for (i = src->minKeyCode; i <= src->maxKeyCode; i++) {
#define SI(r, c) (((r - src->minKeyCode) * src->mapWidth) + (c))
#define DI(r, c) (((r - dst->minKeyCode) * dst->mapWidth) + (c))
	    for (j = 0; j < src->mapWidth; j++)
		dst->map[DI(i, j)] = src->map[SI(i, j)];
	    for (j = src->mapWidth; j < dst->mapWidth; j++)
		dst->map[DI(i, j)] = NoSymbol;
#undef SI
#undef DI
	}
	return TRUE;
    }
    else if (src->mapWidth > dst->mapWidth) {
        i = sizeof(KeySym) * src->mapWidth *
             (dst->maxKeyCode - dst->minKeyCode + 1);
        tmp = xcalloc(sizeof(KeySym), i);
        if (!tmp)
            return FALSE;

        if (dst->map) {
            for (i = 0; i <= dst->maxKeyCode-dst->minKeyCode; i++)
                memmove(&tmp[i * src->mapWidth], &dst->map[i * dst->mapWidth],
                        dst->mapWidth * sizeof(KeySym));
            xfree(dst->map);
        }
        dst->mapWidth = src->mapWidth;
        dst->map = tmp;
    }
    else if (!dst->map) {
        i = sizeof(KeySym) * src->mapWidth *
             (dst->maxKeyCode - dst->minKeyCode + 1);
        tmp = xcalloc(sizeof(KeySym), i);
        if (!tmp)
            return FALSE;

        dst->map = tmp;
        dst->mapWidth = src->mapWidth;
    }

    memmove(&dst->map[rowDif * dst->mapWidth], src->map,
            (src->maxKeyCode - src->minKeyCode + 1) *
            dst->mapWidth * sizeof(KeySym));

    return TRUE;
}

Bool
InitButtonClassDeviceStruct(DeviceIntPtr dev, int numButtons, Atom* labels,
                            CARD8 *map)
{
    ButtonClassPtr butc;
    int i;

    butc = xcalloc(1, sizeof(ButtonClassRec));
    if (!butc)
	return FALSE;
    butc->numButtons = numButtons;
    butc->sourceid = dev->id;
    for (i = 1; i <= numButtons; i++)
	butc->map[i] = map[i];
    for (i = numButtons + 1; i < MAP_LENGTH; i++)
        butc->map[i] = i;
    memcpy(butc->labels, labels, numButtons * sizeof(Atom));
    dev->button = butc;
    return TRUE;
}

Bool
InitValuatorClassDeviceStruct(DeviceIntPtr dev, int numAxes, Atom *labels,
                              int numMotionEvents, int mode)
{
    int i;
    ValuatorClassPtr valc;

    if (!dev)
        return FALSE;

    if (numAxes >= MAX_VALUATORS)
    {
        LogMessage(X_WARNING,
                   "Device '%s' has %d axes, only using first %d.\n",
                   dev->name, numAxes, MAX_VALUATORS);
        numAxes = MAX_VALUATORS;
    }

    valc = (ValuatorClassPtr)xcalloc(1, sizeof(ValuatorClassRec) +
				    numAxes * sizeof(AxisInfo) +
				    numAxes * sizeof(double));
    if (!valc)
	return FALSE;

    valc->sourceid = dev->id;
    valc->motion = NULL;
    valc->first_motion = 0;
    valc->last_motion = 0;

    valc->numMotionEvents = numMotionEvents;
    valc->motionHintWindow = NullWindow;
    valc->numAxes = numAxes;
    valc->mode = mode;
    valc->axes = (AxisInfoPtr)(valc + 1);
    valc->axisVal = (double *)(valc->axes + numAxes);
    dev->valuator = valc;

    AllocateMotionHistory(dev);

    for (i=0; i<numAxes; i++) {
        InitValuatorAxisStruct(dev, i, labels[i], NO_AXIS_LIMITS, NO_AXIS_LIMITS,
                               0, 0, 0);
	valc->axisVal[i]=0;
    }

    dev->last.numValuators = numAxes;

    if (IsMaster(dev) || /* do not accelerate master or xtest devices */
        IsXTestDevice(dev, NULL))
	InitPointerAccelerationScheme(dev, PtrAccelNoOp);
    else
	InitPointerAccelerationScheme(dev, PtrAccelDefault);
    return TRUE;
}

/* global list of acceleration schemes */
ValuatorAccelerationRec pointerAccelerationScheme[] = {
    {PtrAccelNoOp,        NULL, NULL, NULL},
    {PtrAccelPredictable, acceleratePointerPredictable, NULL, AccelerationDefaultCleanup},
    {PtrAccelLightweight, acceleratePointerLightweight, NULL, NULL},
    {-1, NULL, NULL, NULL} /* terminator */
};

/**
 * install an acceleration scheme. returns TRUE on success, and should not
 * change anything if unsuccessful.
 */
Bool
InitPointerAccelerationScheme(DeviceIntPtr dev,
                              int scheme)
{
    int x, i = -1;
    void* data = NULL;
    ValuatorClassPtr val;

    val = dev->valuator;

    if(!val)
	return FALSE;

    if(IsMaster(dev) && scheme != PtrAccelNoOp)
        return FALSE;

    for(x = 0; pointerAccelerationScheme[x].number >= 0; x++) {
        if(pointerAccelerationScheme[x].number == scheme){
            i = x;
            break;
        }
    }

    if(-1 == i)
        return FALSE;

    if (val->accelScheme.AccelCleanupProc)
        val->accelScheme.AccelCleanupProc(dev);

    /* init scheme-specific data */
    switch(scheme){
        case PtrAccelPredictable:
        {
            DeviceVelocityPtr s;
            s = xalloc(sizeof(DeviceVelocityRec));
            if(!s)
        	return FALSE;
            InitVelocityData(s);
            data = s;
            break;
        }
        default:
            break;
    }

    val->accelScheme = pointerAccelerationScheme[i];
    val->accelScheme.accelData = data;

    /* post-init scheme */
    switch(scheme){
        case PtrAccelPredictable:
            InitializePredictableAccelerationProperties(dev);
            break;

        default:
            break;
    }

    return TRUE;
}

Bool
InitAbsoluteClassDeviceStruct(DeviceIntPtr dev)
{
    AbsoluteClassPtr abs;

    abs = xalloc(sizeof(AbsoluteClassRec));
    if (!abs)
        return FALSE;

    /* we don't do anything sensible with these, but should */
    abs->min_x = NO_AXIS_LIMITS;
    abs->min_y = NO_AXIS_LIMITS;
    abs->max_x = NO_AXIS_LIMITS;
    abs->max_y = NO_AXIS_LIMITS;
    abs->flip_x = 0;
    abs->flip_y = 0;
    abs->rotation = 0;
    abs->button_threshold = 0;

    abs->offset_x = 0;
    abs->offset_y = 0;
    abs->width = NO_AXIS_LIMITS;
    abs->height = NO_AXIS_LIMITS;
    abs->following = 0;
    abs->screen = 0;

    abs->sourceid = dev->id;

    dev->absolute = abs;

    return TRUE;
}

Bool
InitFocusClassDeviceStruct(DeviceIntPtr dev)
{
    FocusClassPtr focc;

    focc = xalloc(sizeof(FocusClassRec));
    if (!focc)
	return FALSE;
    focc->win = PointerRootWin;
    focc->revert = None;
    focc->time = currentTime;
    focc->trace = (WindowPtr *)NULL;
    focc->traceSize = 0;
    focc->traceGood = 0;
    focc->sourceid = dev->id;
    dev->focus = focc;
    return TRUE;
}

Bool
InitPtrFeedbackClassDeviceStruct(DeviceIntPtr dev, PtrCtrlProcPtr controlProc)
{
    PtrFeedbackPtr feedc;

    feedc = xalloc(sizeof(PtrFeedbackClassRec));
    if (!feedc)
	return FALSE;
    feedc->CtrlProc = controlProc;
    feedc->ctrl = defaultPointerControl;
    feedc->ctrl.id = 0;
    if ( (feedc->next = dev->ptrfeed) )
        feedc->ctrl.id = dev->ptrfeed->ctrl.id + 1;
    dev->ptrfeed = feedc;
    (*controlProc)(dev, &feedc->ctrl);
    return TRUE;
}


static LedCtrl defaultLedControl = {
	DEFAULT_LEDS, DEFAULT_LEDS_MASK, 0};

static BellCtrl defaultBellControl = {
	DEFAULT_BELL,
	DEFAULT_BELL_PITCH,
	DEFAULT_BELL_DURATION,
	0};

static IntegerCtrl defaultIntegerControl = {
	DEFAULT_INT_RESOLUTION,
	DEFAULT_INT_MIN_VALUE,
	DEFAULT_INT_MAX_VALUE,
	DEFAULT_INT_DISPLAYED,
	0};

Bool
InitStringFeedbackClassDeviceStruct (
      DeviceIntPtr dev, StringCtrlProcPtr controlProc,
      int max_symbols, int num_symbols_supported, KeySym *symbols)
{
    int i;
    StringFeedbackPtr feedc;

    feedc = xalloc(sizeof(StringFeedbackClassRec));
    if (!feedc)
	return FALSE;
    feedc->CtrlProc = controlProc;
    feedc->ctrl.num_symbols_supported = num_symbols_supported;
    feedc->ctrl.num_symbols_displayed = 0;
    feedc->ctrl.max_symbols = max_symbols;
    feedc->ctrl.symbols_supported = xalloc (sizeof (KeySym) * num_symbols_supported);
    feedc->ctrl.symbols_displayed = xalloc (sizeof (KeySym) * max_symbols);
    if (!feedc->ctrl.symbols_supported || !feedc->ctrl.symbols_displayed)
    {
	if (feedc->ctrl.symbols_supported)
	    xfree(feedc->ctrl.symbols_supported);
	if (feedc->ctrl.symbols_displayed)
	    xfree(feedc->ctrl.symbols_displayed);
	xfree(feedc);
	return FALSE;
    }
    for (i=0; i<num_symbols_supported; i++)
	*(feedc->ctrl.symbols_supported+i) = *symbols++;
    for (i=0; i<max_symbols; i++)
	*(feedc->ctrl.symbols_displayed+i) = (KeySym) 0;
    feedc->ctrl.id = 0;
    if ( (feedc->next = dev->stringfeed) )
	feedc->ctrl.id = dev->stringfeed->ctrl.id + 1;
    dev->stringfeed = feedc;
    (*controlProc)(dev, &feedc->ctrl);
    return TRUE;
}

Bool
InitBellFeedbackClassDeviceStruct (DeviceIntPtr dev, BellProcPtr bellProc,
                                   BellCtrlProcPtr controlProc)
{
    BellFeedbackPtr feedc;

    feedc = xalloc(sizeof(BellFeedbackClassRec));
    if (!feedc)
	return FALSE;
    feedc->CtrlProc = controlProc;
    feedc->BellProc = bellProc;
    feedc->ctrl = defaultBellControl;
    feedc->ctrl.id = 0;
    if ( (feedc->next = dev->bell) )
	feedc->ctrl.id = dev->bell->ctrl.id + 1;
    dev->bell = feedc;
    (*controlProc)(dev, &feedc->ctrl);
    return TRUE;
}

Bool
InitLedFeedbackClassDeviceStruct (DeviceIntPtr dev, LedCtrlProcPtr controlProc)
{
    LedFeedbackPtr feedc;

    feedc = xalloc(sizeof(LedFeedbackClassRec));
    if (!feedc)
	return FALSE;
    feedc->CtrlProc = controlProc;
    feedc->ctrl = defaultLedControl;
    feedc->ctrl.id = 0;
    if ( (feedc->next = dev->leds) )
	feedc->ctrl.id = dev->leds->ctrl.id + 1;
    feedc->xkb_sli= NULL;
    dev->leds = feedc;
    (*controlProc)(dev, &feedc->ctrl);
    return TRUE;
}

Bool
InitIntegerFeedbackClassDeviceStruct (DeviceIntPtr dev, IntegerCtrlProcPtr controlProc)
{
    IntegerFeedbackPtr feedc;

    feedc = xalloc(sizeof(IntegerFeedbackClassRec));
    if (!feedc)
	return FALSE;
    feedc->CtrlProc = controlProc;
    feedc->ctrl = defaultIntegerControl;
    feedc->ctrl.id = 0;
    if ( (feedc->next = dev->intfeed) )
	feedc->ctrl.id = dev->intfeed->ctrl.id + 1;
    dev->intfeed = feedc;
    (*controlProc)(dev, &feedc->ctrl);
    return TRUE;
}

Bool
InitPointerDeviceStruct(DevicePtr device, CARD8 *map, int numButtons, Atom* btn_labels,
                        PtrCtrlProcPtr controlProc, int numMotionEvents,
                        int numAxes, Atom *axes_labels)
{
    DeviceIntPtr dev = (DeviceIntPtr)device;

    return(InitButtonClassDeviceStruct(dev, numButtons, btn_labels, map) &&
	   InitValuatorClassDeviceStruct(dev, numAxes, axes_labels,
					 numMotionEvents, 0) &&
	   InitPtrFeedbackClassDeviceStruct(dev, controlProc));
}

/*
 * Check if the given buffer contains elements between low (inclusive) and
 * high (inclusive) only.
 *
 * @return TRUE if the device map is invalid, FALSE otherwise.
 */
Bool
BadDeviceMap(BYTE *buff, int length, unsigned low, unsigned high, XID *errval)
{
    int i;

    for (i = 0; i < length; i++)
	if (buff[i])		       /* only check non-zero elements */
	{
	    if ((low > buff[i]) || (high < buff[i]))
	    {
		*errval = buff[i];
		return TRUE;
	    }
	}
    return FALSE;
}

int
ProcSetModifierMapping(ClientPtr client)
{
    xSetModifierMappingReply rep;
    int rc;
    REQUEST(xSetModifierMappingReq);
    REQUEST_AT_LEAST_SIZE(xSetModifierMappingReq);

    if (client->req_len != ((stuff->numKeyPerModifier << 1) +
                bytes_to_int32(sizeof(xSetModifierMappingReq))))
	return BadLength;

    rep.type = X_Reply;
    rep.length = 0;
    rep.sequenceNumber = client->sequence;

    rc = change_modmap(client, PickKeyboard(client), (KeyCode *)&stuff[1],
                       stuff->numKeyPerModifier);
    if (rc == MappingFailed || rc == -1)
        return BadValue;
    if (rc != Success && rc != MappingSuccess && rc != MappingFailed &&
        rc != MappingBusy)
	return rc;

    rep.success = rc;

    WriteReplyToClient(client, sizeof(xSetModifierMappingReply), &rep);
    return client->noClientException;
}

int
ProcGetModifierMapping(ClientPtr client)
{
    xGetModifierMappingReply rep;
    int max_keys_per_mod = 0;
    KeyCode *modkeymap = NULL;
    REQUEST_SIZE_MATCH(xReq);

    generate_modkeymap(client, PickKeyboard(client), &modkeymap,
                       &max_keys_per_mod);

    memset(&rep, 0, sizeof(xGetModifierMappingReply));
    rep.type = X_Reply;
    rep.numKeyPerModifier = max_keys_per_mod;
    rep.sequenceNumber = client->sequence;
    /* length counts 4 byte quantities - there are 8 modifiers 1 byte big */
    rep.length = max_keys_per_mod << 1;

    WriteReplyToClient(client, sizeof(xGetModifierMappingReply), &rep);
    (void)WriteToClient(client, max_keys_per_mod * 8, (char *) modkeymap);

    xfree(modkeymap);

    return client->noClientException;
}

int
ProcChangeKeyboardMapping(ClientPtr client)
{
    REQUEST(xChangeKeyboardMappingReq);
    unsigned len;
    KeySymsRec keysyms;
    DeviceIntPtr pDev, tmp;
    int rc;
    REQUEST_AT_LEAST_SIZE(xChangeKeyboardMappingReq);

    len = client->req_len - bytes_to_int32(sizeof(xChangeKeyboardMappingReq));
    if (len != (stuff->keyCodes * stuff->keySymsPerKeyCode))
            return BadLength;

    pDev = PickKeyboard(client);

    if ((stuff->firstKeyCode < pDev->key->xkbInfo->desc->min_key_code) ||
	(stuff->firstKeyCode > pDev->key->xkbInfo->desc->max_key_code)) {
	    client->errorValue = stuff->firstKeyCode;
	    return BadValue;

    }
    if (((unsigned)(stuff->firstKeyCode + stuff->keyCodes - 1) >
          pDev->key->xkbInfo->desc->max_key_code) ||
        (stuff->keySymsPerKeyCode == 0)) {
	    client->errorValue = stuff->keySymsPerKeyCode;
	    return BadValue;
    }

    keysyms.minKeyCode = stuff->firstKeyCode;
    keysyms.maxKeyCode = stuff->firstKeyCode + stuff->keyCodes - 1;
    keysyms.mapWidth = stuff->keySymsPerKeyCode;
    keysyms.map = (KeySym *) &stuff[1];

    rc = XaceHook(XACE_DEVICE_ACCESS, client, pDev, DixManageAccess);
    if (rc != Success)
        return rc;

    XkbApplyMappingChange(pDev, &keysyms, stuff->firstKeyCode,
                          stuff->keyCodes, NULL, client);

    for (tmp = inputInfo.devices; tmp; tmp = tmp->next) {
        if (IsMaster(tmp) || tmp->u.master != pDev)
            continue;
        if (!tmp->key)
            continue;

        rc = XaceHook(XACE_DEVICE_ACCESS, client, pDev, DixManageAccess);
        if (rc != Success)
            continue;

        XkbApplyMappingChange(tmp, &keysyms, stuff->firstKeyCode,
                              stuff->keyCodes, NULL, client);
    }

    return client->noClientException;
}

int
ProcSetPointerMapping(ClientPtr client)
{
    BYTE *map;
    int ret;
    int i, j;
    DeviceIntPtr ptr = PickPointer(client);
    xSetPointerMappingReply rep;
    REQUEST(xSetPointerMappingReq);
    REQUEST_AT_LEAST_SIZE(xSetPointerMappingReq);

    if (client->req_len !=
            bytes_to_int32(sizeof(xSetPointerMappingReq) + stuff->nElts))
	return BadLength;
    rep.type = X_Reply;
    rep.length = 0;
    rep.sequenceNumber = client->sequence;
    rep.success = MappingSuccess;
    map = (BYTE *)&stuff[1];

    /* So we're bounded here by the number of core buttons.  This check
     * probably wants disabling through XFixes. */
    /* MPX: With ClientPointer, we can return the right number of buttons.
     * Let's just hope nobody changed ClientPointer between GetPointerMapping
     * and SetPointerMapping
     */
    if (stuff->nElts != ptr->button->numButtons) {
	client->errorValue = stuff->nElts;
	return BadValue;
    }

    /* Core protocol specs don't allow for duplicate mappings; this check
     * almost certainly wants disabling through XFixes too. */
    for (i = 0; i < stuff->nElts; i++) {
        for (j = i + 1; j < stuff->nElts; j++) {
            if (map[i] && map[i] == map[j]) {
                client->errorValue = map[i];
                return BadValue;
            }
        }
    }

    ret = ApplyPointerMapping(ptr, map, stuff->nElts, client);
    if (ret == MappingBusy)
        rep.success = ret;
    else if (ret == -1)
        return BadValue;
    else if (ret != Success)
        return ret;

    WriteReplyToClient(client, sizeof(xSetPointerMappingReply), &rep);
    return Success;
}

int
ProcGetKeyboardMapping(ClientPtr client)
{
    xGetKeyboardMappingReply rep;
    DeviceIntPtr kbd = PickKeyboard(client);
    XkbDescPtr xkb;
    KeySymsPtr syms;
    int rc;
    REQUEST(xGetKeyboardMappingReq);
    REQUEST_SIZE_MATCH(xGetKeyboardMappingReq);

    rc = XaceHook(XACE_DEVICE_ACCESS, client, kbd, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    xkb = kbd->key->xkbInfo->desc;

    if ((stuff->firstKeyCode < xkb->min_key_code) ||
        (stuff->firstKeyCode > xkb->max_key_code)) {
	client->errorValue = stuff->firstKeyCode;
	return BadValue;
    }
    if (stuff->firstKeyCode + stuff->count > xkb->max_key_code + 1) {
	client->errorValue = stuff->count;
        return BadValue;
    }

    syms = XkbGetCoreMap(kbd);
    if (!syms)
        return BadAlloc;

    memset(&rep, 0, sizeof(xGetKeyboardMappingReply));
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.keySymsPerKeyCode = syms->mapWidth;
    /* length is a count of 4 byte quantities and KeySyms are 4 bytes */
    rep.length = syms->mapWidth * stuff->count;
    WriteReplyToClient(client, sizeof(xGetKeyboardMappingReply), &rep);
    client->pSwapReplyFunc = (ReplySwapPtr) CopySwap32Write;
    WriteSwappedDataToClient(client,
                             syms->mapWidth * stuff->count * sizeof(KeySym),
                             &syms->map[syms->mapWidth * (stuff->firstKeyCode -
                                                          syms->minKeyCode)]);
    xfree(syms->map);
    xfree(syms);

    return client->noClientException;
}

int
ProcGetPointerMapping(ClientPtr client)
{
    xGetPointerMappingReply rep;
    /* Apps may get different values each time they call GetPointerMapping as
     * the ClientPointer could change. */
    DeviceIntPtr ptr = PickPointer(client);
    ButtonClassPtr butc = ptr->button;
    int rc;
    REQUEST_SIZE_MATCH(xReq);

    rc = XaceHook(XACE_DEVICE_ACCESS, client, ptr, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.nElts = (butc) ? butc->numButtons : 0;
    rep.length = ((unsigned)rep.nElts + (4-1))/4;
    WriteReplyToClient(client, sizeof(xGetPointerMappingReply), &rep);
    if (butc)
        WriteToClient(client, (int)rep.nElts, (char *)&butc->map[1]);
    return Success;
}

void
NoteLedState(DeviceIntPtr keybd, int led, Bool on)
{
    KeybdCtrl *ctrl = &keybd->kbdfeed->ctrl;
    if (on)
	ctrl->leds |= ((Leds)1 << (led - 1));
    else
	ctrl->leds &= ~((Leds)1 << (led - 1));
}

int
Ones(unsigned long mask)             /* HACKMEM 169 */
{
    unsigned long y;

    y = (mask >> 1) &033333333333;
    y = mask - y - ((y >>1) & 033333333333);
    return (((y + (y >> 3)) & 030707070707) % 077);
}

static int
DoChangeKeyboardControl (ClientPtr client, DeviceIntPtr keybd, XID *vlist,
                         BITS32 vmask)
{
#define DO_ALL    (-1)
    KeybdCtrl ctrl;
    int t;
    int led = DO_ALL;
    int key = DO_ALL;
    BITS32 index2;
    int mask = vmask, i;
    XkbEventCauseRec cause;

    ctrl = keybd->kbdfeed->ctrl;
    while (vmask) {
	index2 = (BITS32) lowbit (vmask);
	vmask &= ~index2;
	switch (index2) {
	case KBKeyClickPercent:
	    t = (INT8)*vlist;
	    vlist++;
	    if (t == -1) {
		t = defaultKeyboardControl.click;
            }
	    else if (t < 0 || t > 100) {
		client->errorValue = t;
		return BadValue;
	    }
	    ctrl.click = t;
	    break;
	case KBBellPercent:
	    t = (INT8)*vlist;
	    vlist++;
	    if (t == -1) {
		t = defaultKeyboardControl.bell;
            }
	    else if (t < 0 || t > 100) {
		client->errorValue = t;
		return BadValue;
	    }
	    ctrl.bell = t;
	    break;
	case KBBellPitch:
	    t = (INT16)*vlist;
	    vlist++;
	    if (t == -1) {
		t = defaultKeyboardControl.bell_pitch;
            }
	    else if (t < 0) {
		client->errorValue = t;
		return BadValue;
	    }
	    ctrl.bell_pitch = t;
	    break;
	case KBBellDuration:
	    t = (INT16)*vlist;
	    vlist++;
	    if (t == -1)
		t = defaultKeyboardControl.bell_duration;
	    else if (t < 0) {
		client->errorValue = t;
		return BadValue;
	    }
	    ctrl.bell_duration = t;
	    break;
	case KBLed:
	    led = (CARD8)*vlist;
	    vlist++;
	    if (led < 1 || led > 32) {
		client->errorValue = led;
		return BadValue;
	    }
	    if (!(mask & KBLedMode))
		return BadMatch;
	    break;
	case KBLedMode:
	    t = (CARD8)*vlist;
	    vlist++;
	    if (t == LedModeOff) {
		if (led == DO_ALL)
		    ctrl.leds = 0x0;
		else
		    ctrl.leds &= ~(((Leds)(1)) << (led - 1));
	    }
	    else if (t == LedModeOn) {
		if (led == DO_ALL)
		    ctrl.leds = ~0L;
		else
		    ctrl.leds |= (((Leds)(1)) << (led - 1));
	    }
	    else {
		client->errorValue = t;
		return BadValue;
	    }

            XkbSetCauseCoreReq(&cause,X_ChangeKeyboardControl,client);
            XkbSetIndicators(keybd,((led == DO_ALL) ? ~0L : (1L<<(led-1))),
 			     ctrl.leds, &cause);
            ctrl.leds = keybd->kbdfeed->ctrl.leds;

	    break;
	case KBKey:
	    key = (KeyCode)*vlist;
	    vlist++;
	    if ((KeyCode)key < keybd->key->xkbInfo->desc->min_key_code ||
		(KeyCode)key > keybd->key->xkbInfo->desc->max_key_code) {
		client->errorValue = key;
		return BadValue;
	    }
	    if (!(mask & KBAutoRepeatMode))
		return BadMatch;
	    break;
	case KBAutoRepeatMode:
	    i = (key >> 3);
	    mask = (1 << (key & 7));
	    t = (CARD8)*vlist;
	    vlist++;
            if (key != DO_ALL)
                XkbDisableComputedAutoRepeats(keybd,key);
	    if (t == AutoRepeatModeOff) {
		if (key == DO_ALL)
		    ctrl.autoRepeat = FALSE;
		else
		    ctrl.autoRepeats[i] &= ~mask;
	    }
	    else if (t == AutoRepeatModeOn) {
		if (key == DO_ALL)
		    ctrl.autoRepeat = TRUE;
		else
		    ctrl.autoRepeats[i] |= mask;
	    }
	    else if (t == AutoRepeatModeDefault) {
		if (key == DO_ALL)
		    ctrl.autoRepeat = defaultKeyboardControl.autoRepeat;
		else
		    ctrl.autoRepeats[i] =
			    (ctrl.autoRepeats[i] & ~mask) |
			    (defaultKeyboardControl.autoRepeats[i] & mask);
	    }
	    else {
		client->errorValue = t;
		return BadValue;
	    }
	    break;
	default:
	    client->errorValue = mask;
	    return BadValue;
	}
    }
    keybd->kbdfeed->ctrl = ctrl;

    /* The XKB RepeatKeys control and core protocol global autorepeat */
    /* value are linked	*/
    XkbSetRepeatKeys(keybd, key, keybd->kbdfeed->ctrl.autoRepeat);

    return Success;

#undef DO_ALL
}

/**
 * Changes kbd control on the ClientPointer and all attached SDs.
 */
int
ProcChangeKeyboardControl (ClientPtr client)
{
    XID *vlist;
    BITS32 vmask;
    int ret = Success, error = Success;
    DeviceIntPtr pDev = NULL, keyboard;
    REQUEST(xChangeKeyboardControlReq);

    REQUEST_AT_LEAST_SIZE(xChangeKeyboardControlReq);

    vmask = stuff->mask;
    vlist = (XID *)&stuff[1];

    if (client->req_len != (sizeof(xChangeKeyboardControlReq)>>2)+Ones(vmask))
	return BadLength;

    keyboard = PickKeyboard(client);

    for (pDev = inputInfo.devices; pDev; pDev = pDev->next) {
        if ((pDev == keyboard || (!IsMaster(keyboard) && pDev->u.master == keyboard)) &&
            pDev->kbdfeed && pDev->kbdfeed->CtrlProc) {
            ret = XaceHook(XACE_DEVICE_ACCESS, client, pDev, DixManageAccess);
	    if (ret != Success)
                return ret;
        }
    }

    for (pDev = inputInfo.devices; pDev; pDev = pDev->next) {
        if ((pDev == keyboard || (!IsMaster(keyboard) && pDev->u.master == keyboard)) &&
            pDev->kbdfeed && pDev->kbdfeed->CtrlProc) {
            ret = DoChangeKeyboardControl(client, pDev, vlist, vmask);
            if (ret != Success)
                error = ret;
        }
    }

    return error;
}

int
ProcGetKeyboardControl (ClientPtr client)
{
    int rc, i;
    DeviceIntPtr kbd = PickKeyboard(client);
    KeybdCtrl *ctrl = &kbd->kbdfeed->ctrl;
    xGetKeyboardControlReply rep;
    REQUEST_SIZE_MATCH(xReq);

    rc = XaceHook(XACE_DEVICE_ACCESS, client, kbd, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    rep.type = X_Reply;
    rep.length = 5;
    rep.sequenceNumber = client->sequence;
    rep.globalAutoRepeat = ctrl->autoRepeat;
    rep.keyClickPercent = ctrl->click;
    rep.bellPercent = ctrl->bell;
    rep.bellPitch = ctrl->bell_pitch;
    rep.bellDuration = ctrl->bell_duration;
    rep.ledMask = ctrl->leds;
    for (i = 0; i < 32; i++)
	rep.map[i] = ctrl->autoRepeats[i];
    WriteReplyToClient(client, sizeof(xGetKeyboardControlReply), &rep);
    return Success;
}

int
ProcBell(ClientPtr client)
{
    DeviceIntPtr dev, keybd = PickKeyboard(client);
    int base = keybd->kbdfeed->ctrl.bell;
    int newpercent;
    int rc;
    REQUEST(xBellReq);
    REQUEST_SIZE_MATCH(xBellReq);

    if (stuff->percent < -100 || stuff->percent > 100) {
	client->errorValue = stuff->percent;
	return BadValue;
    }

    /* Seems like no keyboard actually has the BellProc set. Returning
     * BadDevice (previous code) will make apps crash badly. The man pages
     * doesn't say anything about a BadDevice being returned either.
     * So just quietly do nothing and pretend everything has worked.
     */
    if (!keybd->kbdfeed->BellProc)
        return Success;

    newpercent = (base * stuff->percent) / 100;
    if (stuff->percent < 0)
        newpercent = base + newpercent;
    else
	newpercent = base - newpercent + stuff->percent;

    for (dev = inputInfo.devices; dev; dev = dev->next) {
        if ((dev == keybd || (!IsMaster(dev) && dev->u.master == keybd)) &&
            dev->kbdfeed && dev->kbdfeed->BellProc) {

	    rc = XaceHook(XACE_DEVICE_ACCESS, client, dev, DixBellAccess);
	    if (rc != Success)
		return rc;
            XkbHandleBell(FALSE, FALSE, dev, newpercent,
                          &dev->kbdfeed->ctrl, 0, None, NULL, client);
        }
    }

    return Success;
}

int
ProcChangePointerControl(ClientPtr client)
{
    DeviceIntPtr dev, mouse = PickPointer(client);
    PtrCtrl ctrl;		/* might get BadValue part way through */
    int rc;
    REQUEST(xChangePointerControlReq);
    REQUEST_SIZE_MATCH(xChangePointerControlReq);

    if (!mouse->ptrfeed->CtrlProc)
        return BadDevice;

    ctrl = mouse->ptrfeed->ctrl;
    if ((stuff->doAccel != xTrue) && (stuff->doAccel != xFalse)) {
	client->errorValue = stuff->doAccel;
	return(BadValue);
    }
    if ((stuff->doThresh != xTrue) && (stuff->doThresh != xFalse)) {
	client->errorValue = stuff->doThresh;
	return(BadValue);
    }
    if (stuff->doAccel) {
	if (stuff->accelNum == -1) {
	    ctrl.num = defaultPointerControl.num;
        }
	else if (stuff->accelNum < 0) {
	    client->errorValue = stuff->accelNum;
	    return BadValue;
	}
	else {
            ctrl.num = stuff->accelNum;
        }

	if (stuff->accelDenum == -1) {
	    ctrl.den = defaultPointerControl.den;
        }
	else if (stuff->accelDenum <= 0) {
	    client->errorValue = stuff->accelDenum;
	    return BadValue;
	}
	else {
            ctrl.den = stuff->accelDenum;
        }
    }
    if (stuff->doThresh) {
	if (stuff->threshold == -1) {
	    ctrl.threshold = defaultPointerControl.threshold;
        }
	else if (stuff->threshold < 0) {
	    client->errorValue = stuff->threshold;
	    return BadValue;
	}
	else {
            ctrl.threshold = stuff->threshold;
        }
    }

    for (dev = inputInfo.devices; dev; dev = dev->next) {
        if ((dev == mouse || (!IsMaster(dev) && dev->u.master == mouse)) &&
            dev->ptrfeed && dev->ptrfeed->CtrlProc) {
	    rc = XaceHook(XACE_DEVICE_ACCESS, client, dev, DixManageAccess);
	    if (rc != Success)
		return rc;
	}
    }

    for (dev = inputInfo.devices; dev; dev = dev->next) {
        if ((dev == mouse || (!IsMaster(dev) && dev->u.master == mouse)) &&
            dev->ptrfeed && dev->ptrfeed->CtrlProc) {
            dev->ptrfeed->ctrl = ctrl;
            (*dev->ptrfeed->CtrlProc)(dev, &mouse->ptrfeed->ctrl);
        }
    }

    return Success;
}

int
ProcGetPointerControl(ClientPtr client)
{
    DeviceIntPtr ptr = PickPointer(client);
    PtrCtrl *ctrl = &ptr->ptrfeed->ctrl;
    xGetPointerControlReply rep;
    int rc;
    REQUEST_SIZE_MATCH(xReq);

    rc = XaceHook(XACE_DEVICE_ACCESS, client, ptr, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    rep.type = X_Reply;
    rep.length = 0;
    rep.sequenceNumber = client->sequence;
    rep.threshold = ctrl->threshold;
    rep.accelNumerator = ctrl->num;
    rep.accelDenominator = ctrl->den;
    WriteReplyToClient(client, sizeof(xGenericReply), &rep);
    return Success;
}

void
MaybeStopHint(DeviceIntPtr dev, ClientPtr client)
{
    GrabPtr grab = dev->deviceGrab.grab;

    if ((grab && SameClient(grab, client) &&
	 ((grab->eventMask & PointerMotionHintMask) ||
	  (grab->ownerEvents &&
	   (EventMaskForClient(dev->valuator->motionHintWindow, client) &
	    PointerMotionHintMask)))) ||
	(!grab &&
	 (EventMaskForClient(dev->valuator->motionHintWindow, client) &
	  PointerMotionHintMask)))
	dev->valuator->motionHintWindow = NullWindow;
}

int
ProcGetMotionEvents(ClientPtr client)
{
    WindowPtr pWin;
    xTimecoord * coords = (xTimecoord *) NULL;
    xGetMotionEventsReply rep;
    int i, count, xmin, xmax, ymin, ymax, rc;
    unsigned long nEvents;
    DeviceIntPtr mouse = PickPointer(client);
    TimeStamp start, stop;
    REQUEST(xGetMotionEventsReq);
    REQUEST_SIZE_MATCH(xGetMotionEventsReq);

    rc = dixLookupWindow(&pWin, stuff->window, client, DixGetAttrAccess);
    if (rc != Success)
	return rc;
    rc = XaceHook(XACE_DEVICE_ACCESS, client, mouse, DixReadAccess);
    if (rc != Success)
	return rc;

    if (mouse->valuator->motionHintWindow)
	MaybeStopHint(mouse, client);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    nEvents = 0;
    start = ClientTimeToServerTime(stuff->start);
    stop = ClientTimeToServerTime(stuff->stop);
    if ((CompareTimeStamps(start, stop) != LATER) &&
	(CompareTimeStamps(start, currentTime) != LATER) &&
	mouse->valuator->numMotionEvents)
    {
	if (CompareTimeStamps(stop, currentTime) == LATER)
	    stop = currentTime;
	count = GetMotionHistory(mouse, &coords, start.milliseconds,
				 stop.milliseconds, pWin->drawable.pScreen,
                                 TRUE);
	xmin = pWin->drawable.x - wBorderWidth (pWin);
	xmax = pWin->drawable.x + (int)pWin->drawable.width +
		wBorderWidth (pWin);
	ymin = pWin->drawable.y - wBorderWidth (pWin);
	ymax = pWin->drawable.y + (int)pWin->drawable.height +
		wBorderWidth (pWin);
	for (i = 0; i < count; i++)
	    if ((xmin <= coords[i].x) && (coords[i].x < xmax) &&
		    (ymin <= coords[i].y) && (coords[i].y < ymax))
	    {
		coords[nEvents].time = coords[i].time;
		coords[nEvents].x = coords[i].x - pWin->drawable.x;
		coords[nEvents].y = coords[i].y - pWin->drawable.y;
		nEvents++;
	    }
    }
    rep.length = nEvents * bytes_to_int32(sizeof(xTimecoord));
    rep.nEvents = nEvents;
    WriteReplyToClient(client, sizeof(xGetMotionEventsReply), &rep);
    if (nEvents)
    {
	client->pSwapReplyFunc = (ReplySwapPtr) SwapTimeCoordWrite;
	WriteSwappedDataToClient(client, nEvents * sizeof(xTimecoord),
				 (char *)coords);
    }
    if (coords)
	xfree(coords);
    return Success;
}

int
ProcQueryKeymap(ClientPtr client)
{
    xQueryKeymapReply rep;
    int rc, i;
    DeviceIntPtr keybd = PickKeyboard(client);
    CARD8 *down = keybd->key->down;

    REQUEST_SIZE_MATCH(xReq);
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.length = 2;

    rc = XaceHook(XACE_DEVICE_ACCESS, client, keybd, DixReadAccess);
    if (rc != Success && rc != BadAccess)
	return rc;

    for (i = 0; i<32; i++)
	rep.map[i] = down[i];

    if (rc == BadAccess)
	memset(rep.map, 0, 32);

    WriteReplyToClient(client, sizeof(xQueryKeymapReply), &rep);

   return Success;
}


/**
 * Recalculate the number of buttons for the master device. The number of
 * buttons on the master device is equal to the number of buttons on the
 * slave device with the highest number of buttons.
 */
static void
RecalculateMasterButtons(DeviceIntPtr slave)
{
    DeviceIntPtr dev, master;
    int maxbuttons = 0;

    if (!slave->button || IsMaster(slave))
        return;

    master = GetMaster(slave, MASTER_POINTER);
    if (!master)
        return;

    for (dev = inputInfo.devices; dev; dev = dev->next)
    {
        if (IsMaster(dev) ||
            dev->u.master != master ||
            !dev->button)
            continue;

        maxbuttons = max(maxbuttons, dev->button->numButtons);
    }

    if (master->button->numButtons != maxbuttons)
    {
        int i;
        DeviceChangedEvent event;

        memset(&event, 0, sizeof(event));

        master->button->numButtons = maxbuttons;

        event.header = ET_Internal;
        event.type = ET_DeviceChanged;
        event.time = CurrentTime;
        event.deviceid = master->id;
        event.flags = DEVCHANGE_POINTER_EVENT | DEVCHANGE_DEVICE_CHANGE;
        event.buttons.num_buttons = maxbuttons;
        memcpy(&event.buttons.names, master->button->labels, maxbuttons *
                sizeof(Atom));

        if (master->valuator)
        {
            event.num_valuators = master->valuator->numAxes;
            for (i = 0; i < event.num_valuators; i++)
            {
                event.valuators[i].min = master->valuator->axes[i].min_value;
                event.valuators[i].max = master->valuator->axes[i].max_value;
                event.valuators[i].resolution = master->valuator->axes[i].resolution;
                /* This should, eventually, be a per-axis mode */
                event.valuators[i].mode = master->valuator->mode;
                event.valuators[i].name = master->valuator->axes[i].label;
            }
        }

        if (master->key)
        {
            event.keys.min_keycode = master->key->xkbInfo->desc->min_key_code;
            event.keys.max_keycode = master->key->xkbInfo->desc->max_key_code;
        }

        XISendDeviceChangedEvent(master, master, &event);
    }
}

/**
 * Attach device 'dev' to device 'master'.
 * Client is set to the client that issued the request, or NULL if it comes
 * from some internal automatic pairing.
 *
 * Master may be NULL to set the device floating.
 *
 * We don't allow multi-layer hierarchies right now. You can't attach a slave
 * to another slave.
 */
int
AttachDevice(ClientPtr client, DeviceIntPtr dev, DeviceIntPtr master)
{
    ScreenPtr screen;
    DeviceIntPtr oldmaster;
    if (!dev || IsMaster(dev))
        return BadDevice;

    if (master && !IsMaster(master)) /* can't attach to slaves */
        return BadDevice;

    /* set from floating to floating? */
    if (!dev->u.master && !master && dev->enabled)
        return Success;

    /* free the existing sprite. */
    if (!dev->u.master && dev->spriteInfo->paired == dev)
    {
        screen = miPointerGetScreen(dev);
        screen->DeviceCursorCleanup(dev, screen);
        xfree(dev->spriteInfo->sprite);
    }

    oldmaster = dev->u.master;
    dev->u.master = master;

    /* If device is set to floating, we need to create a sprite for it,
     * otherwise things go bad. However, we don't want to render the cursor,
     * so we reset spriteOwner.
     * Sprite has to be forced to NULL first, otherwise InitializeSprite won't
     * alloc new memory but overwrite the previous one.
     */
    if (!master)
    {
        WindowPtr currentRoot;

        if (dev->spriteInfo->sprite)
            currentRoot = dev->spriteInfo->sprite->spriteTrace[0];
        else /* new device auto-set to floating */
            currentRoot = WindowTable[0];

        /* we need to init a fake sprite */
        screen = currentRoot->drawable.pScreen;
        screen->DeviceCursorInitialize(dev, screen);
        dev->spriteInfo->sprite = NULL;
        InitializeSprite(dev, currentRoot);
        dev->spriteInfo->spriteOwner = FALSE;
        dev->spriteInfo->paired = dev;
    } else
    {
        dev->spriteInfo->sprite = master->spriteInfo->sprite;
        dev->spriteInfo->paired = master;
        dev->spriteInfo->spriteOwner = FALSE;

        RecalculateMasterButtons(master);
    }

    /* XXX: in theory, the MD should change back to its old, original
     * classes when the last SD is detached. Thanks to the XTEST devices,
     * we'll always have an SD attached until the MD is removed.
     * So let's not worry about that.
     */

    return Success;
}

/**
 * Return the device paired with the given device or NULL.
 * Returns the device paired with the parent master if the given device is a
 * slave device.
 */
DeviceIntPtr
GetPairedDevice(DeviceIntPtr dev)
{
    if (!IsMaster(dev) && dev->u.master)
        dev = dev->u.master;

    return dev->spriteInfo->paired;
}


/**
 * Returns the right master for the type of event needed. If the event is a
 * keyboard event.
 * This function may be called with a master device as argument. If so, the
 * returned master is either the device itself or the paired master device.
 * If dev is a floating slave device, NULL is returned.
 *
 * @type ::MASTER_KEYBOARD or ::MASTER_POINTER
 */
DeviceIntPtr
GetMaster(DeviceIntPtr dev, int which)
{
    DeviceIntPtr master;

    if (IsMaster(dev))
        master = dev;
    else
        master = dev->u.master;

    if (master)
    {
        if (which == MASTER_KEYBOARD)
        {
            if (master->type != MASTER_KEYBOARD)
                master = GetPairedDevice(master);
        } else
        {
            if (master->type != MASTER_POINTER)
                master = GetPairedDevice(master);
        }
    }

    return master;
}

/**
 * Create a new device pair (== one pointer, one keyboard device).
 * Only allocates the devices, you will need to call ActivateDevice() and
 * EnableDevice() manually.
 * Either a master or a slave device can be created depending on
 * the value for master.
 */
int
AllocDevicePair (ClientPtr client, char* name,
                 DeviceIntPtr* ptr,
                 DeviceIntPtr* keybd,
                 DeviceProc ptr_proc,
                 DeviceProc keybd_proc,
                 Bool master)
{
    DeviceIntPtr pointer;
    DeviceIntPtr keyboard;
    ClassesPtr classes;
    *ptr = *keybd = NULL;

    pointer = AddInputDevice(client, ptr_proc, TRUE);
    if (!pointer)
        return BadAlloc;

    pointer->name = xcalloc(strlen(name) + strlen(" pointer") + 1, sizeof(char));
    strcpy(pointer->name, name);
    strcat(pointer->name, " pointer");

    pointer->public.processInputProc = ProcessOtherEvent;
    pointer->public.realInputProc = ProcessOtherEvent;
    XkbSetExtension(pointer, ProcessPointerEvent);
    pointer->deviceGrab.ActivateGrab = ActivatePointerGrab;
    pointer->deviceGrab.DeactivateGrab = DeactivatePointerGrab;
    pointer->coreEvents = TRUE;
    pointer->spriteInfo->spriteOwner = TRUE;

    pointer->u.lastSlave = NULL;
    pointer->last.slave = NULL;
    pointer->type = (master) ? MASTER_POINTER : SLAVE;

    keyboard = AddInputDevice(client, keybd_proc, TRUE);
    if (!keyboard)
    {
        RemoveDevice(pointer, FALSE);
        return BadAlloc;
    }

    keyboard->name = xcalloc(strlen(name) + strlen(" keyboard") + 1, sizeof(char));
    strcpy(keyboard->name, name);
    strcat(keyboard->name, " keyboard");

    keyboard->public.processInputProc = ProcessOtherEvent;
    keyboard->public.realInputProc = ProcessOtherEvent;
    XkbSetExtension(keyboard, ProcessKeyboardEvent);
    keyboard->deviceGrab.ActivateGrab = ActivateKeyboardGrab;
    keyboard->deviceGrab.DeactivateGrab = DeactivateKeyboardGrab;
    keyboard->coreEvents = TRUE;
    keyboard->spriteInfo->spriteOwner = FALSE;

    keyboard->u.lastSlave = NULL;
    keyboard->last.slave = NULL;
    keyboard->type = (master) ? MASTER_KEYBOARD : SLAVE;


    /* The ClassesRec stores the device classes currently not used. */
    classes = xcalloc(1, sizeof(ClassesRec));
    dixSetPrivate(&pointer->devPrivates, UnusedClassesPrivateKey, classes);
    classes = xcalloc(1, sizeof(ClassesRec));
    dixSetPrivate(&keyboard->devPrivates, UnusedClassesPrivateKey, classes);

    *ptr = pointer;
    *keybd = keyboard;

    return Success;
}

