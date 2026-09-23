//
//  BootBeacon.cpp
//  Diagnose-Kext fuer Hackintosh-Bring-up ohne Bild und ohne serielle Konsole.
//
//  1) Status-LED (ThinkPad-EC) blinkt als Herzschlag, solange der Kernel lebt.
//     Blinkrate zeigt die Boot-Stufe:  langsam = Kext laeuft,
//                                      mittel  = Root-Dateisystem gemountet,
//                                      schnell = launchd (PID 1) laeuft.
//     Bleibt die LED stehen (an oder aus) -> genau dann ist der Kernel eingefroren.
//  2) Piepser (ThinkPad-EC) bei jeder neuen Stufe.
//  3) "Blackbox": die letzten ~3 KB des Kernel-Logs (msgbuf) werden alle 10 s
//     in eine NVRAM-Variable geschrieben. Nach dem Freeze unter Linux auslesbar:
//     /sys/firmware/efi/efivars/bootbeacon-log-7c436110-ab2a-4bbb-a880-fe41995c9f82
//
//  Boot-Args:
//    -bbeacoff        Kext komplett aus
//    -bbeacdbg        Lilu-Debug-Ausgabe
//    -bbnobeep        keine Piepser
//    -bbnonvram       keine NVRAM-Schreibzugriffe
//    bbled=<maske>    LED-Bitmaske (Default 0x401 = Power-LED (0) + Deckel-i-Punkt (10))
//    bbint=<sek>      NVRAM-Intervall in Sekunden (Default 10)
//    bbmax=<n>        max. Anzahl NVRAM-Schreibvorgaenge (Default 90)
//    bbsize=<bytes>   Groesse der NVRAM-Variable (Default 3072, max 7680)
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <sys/vnode.h>
#include <sys/proc.h>

// ---------------------------------------------------------------------------
// Kernel-Log-Puffer (xnu bsd/sys/msgbuf.h). Nicht exportiert -> Lilu loest
// das Symbol _msgbufp aus der Kernel-Symboltabelle auf.
// ---------------------------------------------------------------------------
struct bb_msgbuf {
	int32_t msg_magic;
	int32_t msg_size;
	int32_t msg_bufx;   // Schreibposition
	int32_t msg_bufr;   // Leseposition (syslogd)
	char   *msg_bufc;   // Ringpuffer
};
static constexpr int32_t BB_MSG_MAGIC = 0x063061;
static bb_msgbuf **gMsgbufp = nullptr;

#define BB_GUID      "7C436110-AB2A-4BBB-A880-FE41995C9F82"
#define BB_VARNAME   BB_GUID ":bootbeacon-log"

// Stufen
enum : uint32_t {
	StageNone     = 0,
	StageEC       = 1,   // Kext + EC erreichbar
	StageRootFS   = 2,   // Root-Vnode vorhanden
	StageLaunchd  = 3,   // PID 1 existiert
};

// Einstellungen (aus Boot-Args)
static uint32_t gLedMask   = (1U << 0) | (1U << 10);
static uint32_t gInterval  = 10;
static uint32_t gMaxWrites = 90;
static uint32_t gVarSize   = 3072;
static bool     gNoBeep    = false;
static bool     gNoNvram   = false;

// ---------------------------------------------------------------------------
// Lilu-Plugin-Teil: nur Symbolaufloesung
// ---------------------------------------------------------------------------
static void onPatcherLoad(void *, KernelPatcher &patcher) {
	auto addr = patcher.solveSymbol(KernelPatcher::KernelID, "_msgbufp");
	if (addr) {
		gMsgbufp = reinterpret_cast<bb_msgbuf **>(addr);
		SYSLOG("bbeac", "msgbufp @ 0x%llx", static_cast<unsigned long long>(addr));
	} else {
		SYSLOG("bbeac", "_msgbufp nicht gefunden (%d) - NVRAM-Log nur mit Header", static_cast<int>(patcher.getError()));
		patcher.clearError();
	}
}

static void pluginStart() {
	uint32_t v;
	if (PE_parse_boot_argn("bbled", &v, sizeof(v)))  gLedMask = v;
	if (PE_parse_boot_argn("bbint", &v, sizeof(v)) && v >= 2) gInterval = v;
	if (PE_parse_boot_argn("bbmax", &v, sizeof(v)))  gMaxWrites = v;
	if (PE_parse_boot_argn("bbsize", &v, sizeof(v)) && v >= 256 && v <= 7680) gVarSize = v;
	gNoBeep  = checkKernelArgument("-bbnobeep");
	gNoNvram = checkKernelArgument("-bbnonvram");
	SYSLOG("bbeac", "start led=0x%x int=%u max=%u size=%u", gLedMask, gInterval, gMaxWrites, gVarSize);
	lilu.onPatcherLoadForce(onPatcherLoad);
}

static const char *bootargOff[] { "-bbeacoff" };
static const char *bootargDebug[] { "-bbeacdbg" };
static const char *bootargBeta[] { "-bbeacbeta" };

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	bootargOff,   arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta,  arrsize(bootargBeta),
	KernelVersion::HighSierra,
	KernelVersion::Tahoe,
	pluginStart
};

// ---------------------------------------------------------------------------
// EC-Teil: haengt sich an das ACPI-EC-Device (PNP0C09)
// ---------------------------------------------------------------------------
class EXPORT BootBeaconEC : public IOService {
	OSDeclareDefaultStructors(BootBeaconEC)

	IOACPIPlatformDevice *ec {nullptr};
	IOWorkLoop *workLoop {nullptr};
	IOTimerEventSource *ledTimer {nullptr};
	IOTimerEventSource *logTimer {nullptr};

	bool     ledState {false};
	uint32_t stage {StageNone};
	uint32_t ticks {0};
	uint32_t writes {0};
	uint32_t nvramFails {0};
	bool     hasLED {false};
	bool     hasBEEP {false};
	char    *buf {nullptr};

	void setLeds(bool on);
	void beep(uint32_t id);
	void checkStage();
	void writeLog(const char *reason);

	static void ledTick(OSObject *owner, IOTimerEventSource *sender);
	static void logTick(OSObject *owner, IOTimerEventSource *sender);

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free() override;
};

OSDefineMetaClassAndStructors(BootBeaconEC, IOService)

static uint64_t uptimeMs() {
	uint64_t abs = 0, ns = 0;
	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000000ULL;
}

static IOReturn evalEC(IOACPIPlatformDevice *ec, const char *method, uint32_t a, bool twoArgs, uint32_t b) {
	OSNumber *p0 = OSNumber::withNumber(a, 32);
	OSNumber *p1 = twoArgs ? OSNumber::withNumber(b, 32) : nullptr;
	if (!p0 || (twoArgs && !p1)) {
		OSSafeReleaseNULL(p0);
		OSSafeReleaseNULL(p1);
		return kIOReturnNoMemory;
	}
	OSObject *params[2] { p0, p1 };
	IOReturn r = ec->evaluateObject(method, nullptr, params, twoArgs ? 2 : 1);
	p0->release();
	OSSafeReleaseNULL(p1);
	return r;
}

void BootBeaconEC::setLeds(bool on) {
	if (!hasLED)
		return;
	for (uint32_t id = 0; id < 16; id++) {
		if (gLedMask & (1U << id))
			evalEC(ec, "LED", id, true, on ? 0x80 : 0x00);
	}
}

void BootBeaconEC::beep(uint32_t id) {
	if (hasBEEP && !gNoBeep)
		evalEC(ec, "BEEP", id, false, 0);
}

void BootBeaconEC::checkStage() {
	if (stage < StageRootFS) {
		vnode_t root = vfs_rootvnode();
		if (root) {
			vnode_put(root);
			stage = StageRootFS;
			IOLog("BootBeacon: Stufe 2 (RootFS) bei %llu ms\n", uptimeMs());
			beep(7);            // hoher Piep
			writeLog("rootfs");
		}
	}
	if (stage == StageRootFS) {
		proc_t p = proc_find(1);
		if (p) {
			proc_rele(p);
			stage = StageLaunchd;
			IOLog("BootBeacon: Stufe 3 (launchd) bei %llu ms\n", uptimeMs());
			beep(9);            // drei kurze Pieps
			writeLog("launchd");
		}
	}
}

void BootBeaconEC::writeLog(const char *reason) {
	if (gNoNvram || !buf || writes >= gMaxWrites)
		return;

	size_t cap = gVarSize;
	int hdr = snprintf(buf, cap,
		"BB1 %s stage=%u t=%llums n=%u fail=%u tick=%u msgbuf=%s\n",
		reason, stage, uptimeMs(), writes + 1, nvramFails, ticks,
		gMsgbufp ? "ok" : "none");
	if (hdr < 0) hdr = 0;
	size_t used = static_cast<size_t>(hdr) < cap ? static_cast<size_t>(hdr) : cap - 1;

	// Letzte Bytes aus dem Ringpuffer kopieren (ohne Lock; nur Momentaufnahme)
	bb_msgbuf *mb = gMsgbufp ? *gMsgbufp : nullptr;
	if (mb && mb->msg_magic == BB_MSG_MAGIC && mb->msg_bufc && mb->msg_size > 0) {
		int32_t size = mb->msg_size;
		int32_t x = mb->msg_bufx;
		const char *ring = mb->msg_bufc;
		if (x >= 0 && x < size) {
			size_t want = cap - used;
			if (want > static_cast<size_t>(size)) want = size;
			int32_t start = x - static_cast<int32_t>(want);
			if (start < 0) start += size;
			for (size_t i = 0; i < want; i++) {
				char c = ring[(start + static_cast<int32_t>(i)) % size];
				buf[used++] = c ? c : ' ';
			}
		}
	}

	bool ok = false;
	IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (opts) {
		const OSSymbol *key = OSSymbol::withCString(BB_VARNAME);
		OSData *data = OSData::withBytes(buf, static_cast<unsigned int>(used));
		if (key && data)
			ok = opts->setProperty(key, data);
		OSSafeReleaseNULL(key);
		OSSafeReleaseNULL(data);
		if (ok) {
			// Sofort auf Flash schreiben (IONVRAM wertet diese Schluessel als Sync-Befehl)
			opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "IONVRAM-FORCESYNCNOW-PROPERTY");
			opts->setProperty("IONVRAM-SYNCNOW-PROPERTY", "IONVRAM-SYNCNOW-PROPERTY");
		}
		opts->release();
	}

	writes++;
	if (!ok) {
		nvramFails++;
		IOLog("BootBeacon: NVRAM-Schreiben fehlgeschlagen (%u)\n", nvramFails);
		if (nvramFails == 1)
			beep(4);            // hoch-tief "unable"
	}
}

void BootBeaconEC::ledTick(OSObject *owner, IOTimerEventSource *sender) {
	auto self = OSDynamicCast(BootBeaconEC, owner);
	if (!self)
		return;
	self->ticks++;
	self->ledState = !self->ledState;
	self->setLeds(self->ledState);
	if (self->stage < StageLaunchd)
		self->checkStage();
	// Halbe Periode: Stufe 1 = 1000 ms, Stufe 2 = 400 ms, Stufe 3 = 150 ms
	uint32_t half = self->stage >= StageLaunchd ? 150 : (self->stage >= StageRootFS ? 400 : 1000);
	sender->setTimeoutMS(half);
}

void BootBeaconEC::logTick(OSObject *owner, IOTimerEventSource *sender) {
	auto self = OSDynamicCast(BootBeaconEC, owner);
	if (!self)
		return;
	self->writeLog("tick");
	if (self->writes < gMaxWrites)
		sender->setTimeoutMS(gInterval * 1000);
}

bool BootBeaconEC::start(IOService *provider) {
	if (checkKernelArgument("-bbeacoff"))
		return false;
	if (!IOService::start(provider))
		return false;

	ec = OSDynamicCast(IOACPIPlatformDevice, provider);
	if (!ec) {
		IOLog("BootBeacon: Provider ist kein IOACPIPlatformDevice\n");
		return false;
	}
	ec->retain();

	hasLED  = ec->validateObject("LED") == kIOReturnSuccess;
	hasBEEP = ec->validateObject("BEEP") == kIOReturnSuccess;
	IOLog("BootBeacon: EC gefunden, LED=%d BEEP=%d, t=%llu ms\n", hasLED, hasBEEP, uptimeMs());
	setProperty("HasLED", hasLED);
	setProperty("HasBEEP", hasBEEP);
	if (!hasLED && !hasBEEP) {
		// Falscher EC (kein ThinkPad-EC) -> nur NVRAM-Log
		IOLog("BootBeacon: EC ohne LED/BEEP-Methoden\n");
	}

	buf = static_cast<char *>(IOMalloc(gVarSize));

	workLoop = IOWorkLoop::workLoop();
	if (!workLoop) {
		IOLog("BootBeacon: kein WorkLoop\n");
		return false;
	}
	ledTimer = IOTimerEventSource::timerEventSource(this, ledTick);
	logTimer = IOTimerEventSource::timerEventSource(this, logTick);
	if (!ledTimer || !logTimer ||
		workLoop->addEventSource(ledTimer) != kIOReturnSuccess ||
		workLoop->addEventSource(logTimer) != kIOReturnSuccess) {
		IOLog("BootBeacon: Timer-Fehler\n");
		return false;
	}

	stage = StageEC;
	beep(5);                        // einzelner Piep: Kext + EC laufen
	writeLog("start");              // sofort ein erster Eintrag
	ledTimer->setTimeoutMS(1000);
	logTimer->setTimeoutMS(gInterval * 1000);

	registerService();
	return true;
}

void BootBeaconEC::stop(IOService *provider) {
	if (ledTimer) ledTimer->cancelTimeout();
	if (logTimer) logTimer->cancelTimeout();
	IOService::stop(provider);
}

void BootBeaconEC::free() {
	if (workLoop) {
		if (ledTimer) workLoop->removeEventSource(ledTimer);
		if (logTimer) workLoop->removeEventSource(logTimer);
	}
	OSSafeReleaseNULL(ledTimer);
	OSSafeReleaseNULL(logTimer);
	OSSafeReleaseNULL(workLoop);
	OSSafeReleaseNULL(ec);
	if (buf) {
		IOFree(buf, gVarSize);
		buf = nullptr;
	}
	IOService::free();
}
