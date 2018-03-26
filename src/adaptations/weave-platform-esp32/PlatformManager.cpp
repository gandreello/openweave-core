#include <internal/WeavePlatformInternal.h>
#include <PlatformManager.h>
#include <internal/DeviceControlServer.h>
#include <internal/DeviceDescriptionServer.h>
#include <internal/FabricProvisioningServer.h>
#include <internal/ServiceProvisioningServer.h>
#include <internal/EchoServer.h>
#include <new>
#include <esp_timer.h>

using namespace ::nl;
using namespace ::nl::Weave;
using namespace ::WeavePlatform::Internal;

namespace WeavePlatform {

namespace Internal {

extern WEAVE_ERROR InitCASEAuthDelegate();
extern int GetEntropy_ESP32(uint8_t *buf, size_t bufSize);

} // namespace Internal

namespace {

SemaphoreHandle_t gLwIPCoreLock;
QueueHandle_t gWeaveEventQueue;
bool gWeaveTimerActive;
TimeOut_t gNextTimerBaseTime;
TickType_t gNextTimerDurationTicks;

} // unnamed namespace


// ==================== PlatformManager Public Members ====================

WEAVE_ERROR PlatformManager::InitLwIPCoreLock()
{
    gLwIPCoreLock = xSemaphoreCreateMutex();
    if (gLwIPCoreLock == NULL) {
        ESP_LOGE(TAG, "Failed to create LwIP core lock");
        return WEAVE_ERROR_NO_MEMORY;
    }

    return WEAVE_NO_ERROR;
}

WEAVE_ERROR PlatformManager::InitWeaveStack()
{
    WEAVE_ERROR err;

    // Initialize the source used by Weave to get secure random data.
    err = nl::Weave::Platform::Security::InitSecureRandomDataSource(GetEntropy_ESP32, 64, NULL, 0);
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Secure random data source initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the master Weave event queue.
    err = InitWeaveEventQueue();
    SuccessOrExit(err);

    // Initialize the Configuration Manager object.
    new (&ConfigurationMgr) ConfigurationManager();
    err = ConfigurationMgr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Configuration Manager initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Weave system layer.
    new (&SystemLayer) System::Layer();
    err = SystemLayer.Init(NULL);
    if (err != WEAVE_SYSTEM_NO_ERROR)
    {
        ESP_LOGE(TAG, "SystemLayer initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Weave Inet layer.
    new (&InetLayer) Inet::InetLayer();
    err = InetLayer.Init(SystemLayer, NULL);
    if (err != INET_NO_ERROR)
    {
        ESP_LOGE(TAG, "InetLayer initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Weave fabric state object.
    new (&FabricState) WeaveFabricState();
    err = FabricState.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "FabricState initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    FabricState.DefaultSubnet = kWeaveSubnetId_PrimaryWiFi;

#if WEAVE_CONFIG_SECURITY_TEST_MODE
    FabricState.LogKeys = true;
#endif

    {
        WeaveMessageLayer::InitContext initContext;
        initContext.systemLayer = &SystemLayer;
        initContext.inet = &InetLayer;
        initContext.fabricState = &FabricState;
        initContext.listenTCP = true;
        initContext.listenUDP = true;

        // Initialize the Weave message layer.
        new (&MessageLayer) WeaveMessageLayer();
        err = MessageLayer.Init(&initContext);
        if (err != WEAVE_NO_ERROR) {
            ESP_LOGE(TAG, "MessageLayer initialization failed: %s", ErrorStr(err));
        }
        SuccessOrExit(err);
    }

    // Initialize the Weave exchange manager.
    err = ExchangeMgr.Init(&MessageLayer);
    if (err != WEAVE_NO_ERROR) {
        ESP_LOGE(TAG, "ExchangeMgr initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Weave security manager.
    new (&SecurityMgr) WeaveSecurityManager();
    err = SecurityMgr.Init(ExchangeMgr, SystemLayer);
    if (err != WEAVE_NO_ERROR) {
        ESP_LOGE(TAG, "SecurityMgr initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    SecurityMgr.IdleSessionTimeout = 30000; // TODO: make configurable
    SecurityMgr.SessionEstablishTimeout = 15000; // TODO: make configurable

    // Initialize the CASE auth delegate object.
    err = InitCASEAuthDelegate();
    SuccessOrExit(err);

#if WEAVE_CONFIG_SECURITY_TEST_MODE
    SecurityMgr.CASEUseKnownECDHKey = true;
#endif

    // Perform dynamic configuration of the Weave stack based on store settings.
    err = ConfigurationMgr.ConfigureWeaveStack();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "FabricState initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Connectivity Manager object.
    new (&ConnectivityMgr) ConnectivityManager();
    err = ConnectivityMgr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Connectivity Manager initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Device Control server.
    new (&DeviceControlSvr) DeviceControlServer();
    err = DeviceControlSvr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Weave Device Control server initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Device Description server.
    new (&DeviceDescriptionSvr) DeviceDescriptionServer();
    err = DeviceDescriptionSvr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Weave Device Control server initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Fabric Provisioning server.
    new (&FabricProvisioningSvr) FabricProvisioningServer();
    err = FabricProvisioningSvr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Weave Fabric Provisionining server initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Service Provisioning server.
    new (&ServiceProvisioningSvr) ServiceProvisioningServer();
    err = ServiceProvisioningSvr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Weave Service Provisionining server initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

    // Initialize the Echo server.
    new (&EchoSvr) EchoServer();
    err = EchoSvr.Init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Weave Echo server initialization failed: %s", ErrorStr(err));
    }
    SuccessOrExit(err);

exit:
    return err;
}

void PlatformManager::RunEventLoop()
{
    WEAVE_ERROR err;
    WeavePlatformEvent event;

    while (true)
    {
        TickType_t waitTime;

        // If one or more Weave timers are active...
        if (gWeaveTimerActive) {

            // Adjust the base time and remaining duration for the next scheduled timer based on the
            // amount of time that has elapsed since it was started.
            // IF the timer's expiration time has already arrived...
            if (xTaskCheckForTimeOut(&gNextTimerBaseTime, &gNextTimerDurationTicks) == pdTRUE) {

                // Reset the 'timer active' flag.  This will be set to true again by HandlePlatformTimer()
                // if there are further timers beyond the expired one that are still active.
                gWeaveTimerActive = false;

                // Call into the system layer to dispatch the callback functions for all timers
                // that have expired.
                err = SystemLayer.HandlePlatformTimer();
                if (err != WEAVE_SYSTEM_NO_ERROR) {
                    ESP_LOGE(TAG, "Error handling Weave timers: %s", ErrorStr(err));
                }

                // When processing the event queue below, do not wait if the queue is empty.  Instead
                // immediately loop around and process timers again
                waitTime = 0;
            }

            // If there is still time before the next timer expires, arrange to wait on the event queue
            // until that timer expires.
            else {
                waitTime = gNextTimerDurationTicks;
            }
        }

        // Otherwise no Weave timers are active, so wait indefinitely for an event to arrive on the event
        // queue.
        else {
            waitTime = portMAX_DELAY;
        }

        // TODO: unlock Weave stack

        BaseType_t eventReceived = xQueueReceive(gWeaveEventQueue, &event, waitTime);

        // TODO: lock Weave stack

        // If an event was received, dispatch it.  Continue receiving events from the queue and
        // dispatching them until the queue is empty.
        while (eventReceived == pdTRUE) {

            DispatchEvent(&event);

            eventReceived = xQueueReceive(gWeaveEventQueue, &event, 0);
        }
    }
}

esp_err_t PlatformManager::HandleESPSystemEvent(void * ctx, system_event_t * espEvent)
{
    if (gWeaveEventQueue != NULL)
    {
        WeavePlatformEvent event;

        event.Type = WeavePlatformEvent::kType_ESPSystemEvent;
        event.ESPSystemEvent = *espEvent;

        if (!xQueueSend(gWeaveEventQueue, &event, 1))
        {
            ESP_LOGE(TAG, "Failed to post ESP system event to Weave Platform event queue");
        }
    }

    return ESP_OK;
}


// ==================== PlatformManager Private Members ====================

WEAVE_ERROR PlatformManager::InitWeaveEventQueue()
{
    // TODO: make queue size configurable
    gWeaveEventQueue = xQueueCreate(100, sizeof(WeavePlatformEvent));
    if (gWeaveEventQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate Weave event queue");
        return WEAVE_ERROR_NO_MEMORY;
    }

    return WEAVE_NO_ERROR;
}

void PlatformManager::DispatchEvent(const WeavePlatformEvent * event)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    // If the event is a Weave System or Inet Layer event, dispatch it to the SystemLayer event handler.
    if (event->Type == WeavePlatformEvent::kType_WeaveSystemEvent)
    {
        err = SystemLayer.HandleEvent(*event->WeaveSystemEvent.Target, event->WeaveSystemEvent.Type, event->WeaveSystemEvent.Argument);
        if (err != WEAVE_SYSTEM_NO_ERROR)
        {
            ESP_LOGE(TAG, "Error handling Weave System Layer event (type %d): %s", event->Type, nl::ErrorStr(err));
        }
    }

    else if (event->Type == WeavePlatformEvent::kType_ESPSystemEvent)
    {
        ConnectivityMgr.OnPlatformEvent(event);
    }
}

} // namespace WeavePlatform


// ==================== LwIP Core Locking Functions ====================

extern "C" void lock_lwip_core()
{
    xSemaphoreTake(::WeavePlatform::gLwIPCoreLock, portMAX_DELAY);
}

extern "C" void unlock_lwip_core()
{
    xSemaphoreGive(::WeavePlatform::gLwIPCoreLock);
}


// ==================== Time / Timer Support Functions ====================

namespace nl {
namespace Weave {
namespace System {
namespace Platform {
namespace Layer {

using namespace ::WeavePlatform;
using namespace ::WeavePlatform::Internal;

uint64_t GetSystemTimeMS(void)
{
    return (uint64_t)::esp_timer_get_time()/1000;
}

System::Error StartTimer(System::Layer & aLayer, void * aContext, uint32_t aMilliseconds)
{
    gWeaveTimerActive = true;
    vTaskSetTimeOutState(&gNextTimerBaseTime);
    gNextTimerDurationTicks = pdMS_TO_TICKS(aMilliseconds);

    // TODO: kick event loop thread if this method is called on a different thread.

    return WEAVE_SYSTEM_NO_ERROR;
}

} // namespace Layer
} // namespace Platform
} // namespace System
} // namespace Weave
} // namespace nl


// ==================== System Layer Event Support Functions ====================

namespace nl {
namespace Weave {
namespace System {
namespace Platform {
namespace Layer {

using namespace ::WeavePlatform;
using namespace ::WeavePlatform::Internal;

System::Error PostEvent(System::Layer & aLayer, void * aContext, System::Object & aTarget, System::EventType aType, uintptr_t aArgument)
{
    Error err = WEAVE_SYSTEM_NO_ERROR;
    WeavePlatformEvent event;

    event.Type = WeavePlatformEvent::kType_WeaveSystemEvent;
    event.WeaveSystemEvent.Type = aType;
    event.WeaveSystemEvent.Target = &aTarget;
    event.WeaveSystemEvent.Argument = aArgument;

    if (!xQueueSend(gWeaveEventQueue, &event, 1)) {
        ESP_LOGE(TAG, "Failed to post event to Weave Platform event queue");
        err = WEAVE_ERROR_NO_MEMORY;
    }

    return err;
}

System::Error DispatchEvents(Layer & aLayer, void * aContext)
{
    PlatformMgr.RunEventLoop();
    return WEAVE_SYSTEM_NO_ERROR;
}

System::Error DispatchEvent(System::Layer & aLayer, void * aContext, const WeavePlatformEvent * aEvent)
{
    PlatformMgr.DispatchEvent(aEvent);
    return WEAVE_NO_ERROR;
}

} // namespace Layer
} // namespace Platform
} // namespace System
} // namespace Weave
} // namespace nl
