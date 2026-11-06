#include "PermissionDevices.h"
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QRegularExpression>
#include <QTimer>
#ifdef PACITUM_HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif
#ifdef PACITUM_HAVE_BLUETOOTH
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothLocalDevice>
#include <QBluetoothUuid>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <hidclass.h>
#endif

namespace PermDevices {

static QString hex4(int v) { return QStringLiteral("%1").arg(v, 4, 16, QLatin1Char('0')); }

#ifdef Q_OS_WIN
static QString regProp(HDEVINFO set, SP_DEVINFO_DATA* d, DWORD prop) {
    wchar_t buf[512]; DWORD type = 0, size = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, d, prop, &type, reinterpret_cast<PBYTE>(buf), sizeof(buf) - 2, &size)) return {};
    buf[qMin<DWORD>(size / 2, 511)] = 0;
    return QString::fromWCharArray(buf);   // REG_MULTI_SZ: first string
}
static QList<QJsonObject> setupDiList(const GUID& guid, const char* kind) {
    QList<QJsonObject> out;
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d; d.cbSize = sizeof(d);
    static const QRegularExpression vidpid(QStringLiteral("VID_([0-9A-Fa-f]{4}).*PID_([0-9A-Fa-f]{4})"));
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t inst[512] = {0};
        SetupDiGetDeviceInstanceIdW(set, &d, inst, 511, nullptr);
        const QString instance = QString::fromWCharArray(inst);
        const QString hw = regProp(set, &d, SPDRP_HARDWAREID);
        QString name = regProp(set, &d, SPDRP_FRIENDLYNAME);
        if (name.isEmpty()) name = regProp(set, &d, SPDRP_DEVICEDESC);
        const QString mfg = regProp(set, &d, SPDRP_MFG);
        const auto m = vidpid.match(hw.isEmpty() ? instance : hw);
        if (!m.hasMatch()) continue;
        const int vid = m.captured(1).toInt(nullptr, 16), pid = m.captured(2).toInt(nullptr, 16);
        const QString serial = instance.section(QLatin1Char('\\'), -1);
        QJsonObject o{{QStringLiteral("vendorId"), vid}, {QStringLiteral("productId"), pid}, {QStringLiteral("name"), name},
                      {QStringLiteral("manufacturer"), mfg}, {QStringLiteral("serial"), serial.contains(QLatin1Char('&')) ? QString() : serial},
                      {QStringLiteral("meta"), hex4(vid) + QLatin1Char(':') + hex4(pid)},
                      {QStringLiteral("id"), QStringLiteral("%1:%2:%3:%4").arg(QLatin1String(kind), hex4(vid), hex4(pid), serial)}};
        out << o;
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}
#else
static QString readSys(const QString& p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString(); }
#endif

static QList<QJsonObject> usbDevices() {
#ifdef Q_OS_WIN
    return setupDiList(GUID_DEVINTERFACE_USB_DEVICE, "usb");
#else
    QList<QJsonObject> out;
    const QDir dir(QStringLiteral("/sys/bus/usb/devices"));
    for (const QString& e : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString b = dir.filePath(e) + QLatin1Char('/');
        const QString v = readSys(b + QStringLiteral("idVendor")), p = readSys(b + QStringLiteral("idProduct"));
        if (v.isEmpty() || readSys(b + QStringLiteral("bDeviceClass")) == QLatin1String("09")) continue;   // skip hubs
        const QString serial = readSys(b + QStringLiteral("serial"));
        QString name = readSys(b + QStringLiteral("product")); if (name.isEmpty()) name = QStringLiteral("USB device");
        out << QJsonObject{{QStringLiteral("vendorId"), v.toInt(nullptr, 16)}, {QStringLiteral("productId"), p.toInt(nullptr, 16)},
                           {QStringLiteral("name"), name}, {QStringLiteral("manufacturer"), readSys(b + QStringLiteral("manufacturer"))},
                           {QStringLiteral("serial"), serial}, {QStringLiteral("meta"), v + QLatin1Char(':') + p},
                           {QStringLiteral("id"), QStringLiteral("usb:%1:%2:%3").arg(v, p, serial.isEmpty() ? e : serial)}};
    }
    return out;
#endif
}

static QList<QJsonObject> hidDevices() {
#ifdef Q_OS_WIN
    QList<QJsonObject> all = setupDiList(GUID_DEVINTERFACE_HID, "hid"), out;
    QSet<QString> seen;   // one entry per physical device (Windows lists every top-level collection)
    for (const auto& d : all) { const QString k = d.value(QStringLiteral("meta")).toString() + d.value(QStringLiteral("name")).toString(); if (!seen.contains(k)) { seen.insert(k); out << d; } }
    return out;
#else
    QList<QJsonObject> out;
    const QDir dir(QStringLiteral("/sys/class/hidraw"));
    static const QRegularExpression re(QStringLiteral("HID_ID=\\w+:0*([0-9A-Fa-f]{1,4}):0*([0-9A-Fa-f]{1,4})"));
    for (const QString& e : dir.entryList(QDir::Dirs | QDir::System | QDir::NoDotAndDotDot)) {
        const QString ue = readSys(dir.filePath(e) + QStringLiteral("/device/uevent"));
        const auto m = re.match(ue); if (!m.hasMatch()) continue;
        QString name; for (const QString& l : ue.split(QLatin1Char('\n'))) if (l.startsWith(QLatin1String("HID_NAME="))) name = l.mid(9);
        const int vid = m.captured(1).toInt(nullptr, 16), pid = m.captured(2).toInt(nullptr, 16);
        out << QJsonObject{{QStringLiteral("vendorId"), vid}, {QStringLiteral("productId"), pid}, {QStringLiteral("name"), name.isEmpty() ? QStringLiteral("HID device") : name},
                           {QStringLiteral("meta"), hex4(vid) + QLatin1Char(':') + hex4(pid)}, {QStringLiteral("id"), QStringLiteral("hid:%1:%2:%3").arg(hex4(vid), hex4(pid), e)}};
    }
    return out;
#endif
}

static QList<QJsonObject> serialPorts() {
    QList<QJsonObject> out;
#ifdef PACITUM_HAVE_SERIALPORT
    for (const QSerialPortInfo& p : QSerialPortInfo::availablePorts()) {
        QJsonObject o{{QStringLiteral("port"), p.portName()}, {QStringLiteral("name"), p.description().isEmpty() ? p.portName() : p.description() + QStringLiteral(" (") + p.portName() + QLatin1Char(')')},
                      {QStringLiteral("meta"), p.portName()}, {QStringLiteral("manufacturer"), p.manufacturer()}, {QStringLiteral("serial"), p.serialNumber()}};
        if (p.hasVendorIdentifier()) { o.insert(QStringLiteral("vendorId"), p.vendorIdentifier()); o.insert(QStringLiteral("productId"), p.productIdentifier());
            o.insert(QStringLiteral("meta"), p.portName() + QStringLiteral(" · ") + hex4(p.vendorIdentifier()) + QLatin1Char(':') + hex4(p.productIdentifier())); }
        // stable id: USB serial number when present (survives COM renumbering), else the port name
        o.insert(QStringLiteral("id"), p.serialNumber().isEmpty() ? QStringLiteral("serial:") + p.portName()
                 : QStringLiteral("serial:%1:%2:%3").arg(hex4(p.vendorIdentifier()), hex4(p.productIdentifier()), p.serialNumber()));
        out << o;
    }
#endif
    return out;
}

QList<QJsonObject> enumerate(Perm::Kind k) {
    switch (k) {
    case Perm::Kind::Usb: return usbDevices();
    case Perm::Kind::Hid: return hidDevices();
    case Perm::Kind::Serial: return serialPorts();
    default: return {};
    }
}

bool matches(Perm::Kind k, const QJsonObject& dev, const QJsonObject& options) {
    const QJsonArray filters = options.value(QStringLiteral("filters")).toArray();
    if (filters.isEmpty()) return true;   // Web Serial / WebHID allow empty filters
    const bool serial = k == Perm::Kind::Serial;
    const int vid = dev.value(QStringLiteral("vendorId")).toInt(-1), pid = dev.value(QStringLiteral("productId")).toInt(-1);
    for (const auto& fv : filters) {
        const QJsonObject f = fv.toObject();
        const QString vk = serial ? QStringLiteral("usbVendorId") : QStringLiteral("vendorId");
        const QString pk = serial ? QStringLiteral("usbProductId") : QStringLiteral("productId");
        if (f.contains(vk) && f.value(vk).toInt() != vid) continue;
        if (f.contains(pk) && f.value(pk).toInt() != pid) continue;
        if (f.contains(QStringLiteral("serialNumber")) && f.value(QStringLiteral("serialNumber")).toString() != dev.value(QStringLiteral("serial")).toString()) continue;
        return true;
    }
    return false;
}

bool bluetoothAvailable() {
#ifdef PACITUM_HAVE_BLUETOOTH
    return !QBluetoothLocalDevice::allDevices().isEmpty();
#else
    return false;
#endif
}

static QString capName(Perm::Kind k) {
    switch (k) {
    case Perm::Kind::Camera: return QStringLiteral("webcam");
    case Perm::Kind::Microphone: return QStringLiteral("microphone");
    case Perm::Kind::Location: return QStringLiteral("location");
    default: return {};
    }
}
bool osDenied(Perm::Kind k) {
    if (qEnvironmentVariableIsSet("PACITUM_TEST_OS_DENIED")) return qEnvironmentVariable("PACITUM_TEST_OS_DENIED").contains(Perm::key(k));
#ifdef Q_OS_WIN
    const QString cap = capName(k);
    if (cap.isEmpty()) return false;
    const QString base = QStringLiteral("\\Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + cap;
    auto denied = [](const QString& path){ QSettings s(path, QSettings::NativeFormat); return s.value(QStringLiteral("Value")).toString().compare(QLatin1String("Deny"), Qt::CaseInsensitive) == 0; };
    // Machine-wide switch, the user's "Camera access" switch, and "Let desktop apps access your camera".
    return denied(QStringLiteral("HKEY_LOCAL_MACHINE") + base) || denied(QStringLiteral("HKEY_CURRENT_USER") + base)
        || denied(QStringLiteral("HKEY_CURRENT_USER") + base + QStringLiteral("\\NonPackaged"));
#else
    Q_UNUSED(k);
    return false;
#endif
}
QString osSettingsUrl(Perm::Kind k) {
#if defined(Q_OS_WIN)
    return k == Perm::Kind::Camera ? QStringLiteral("ms-settings:privacy-webcam") : k == Perm::Kind::Microphone ? QStringLiteral("ms-settings:privacy-microphone") : QStringLiteral("ms-settings:privacy-location");
#else
    Q_UNUSED(k); return {};
#endif
}
QString osName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows");
#else
    return QStringLiteral("the system");
#endif
}
}

BluetoothScanner::BluetoothScanner(const QJsonObject& options, QObject* parent) : QObject(parent), m_opts(options) {}

void BluetoothScanner::start() {
#ifdef PACITUM_HAVE_BLUETOOTH
    if (!PermDevices::bluetoothAvailable()) { QTimer::singleShot(0, this, [this]{ emit finished(QStringLiteral("Bluetooth is off or not available")); }); return; }
    auto* agent = new QBluetoothDeviceDiscoveryAgent(this);
    m_agent = agent;
    agent->setLowEnergyDiscoveryTimeout(15000);
    const bool all = m_opts.value(QStringLiteral("acceptAllDevices")).toBool();
    const QJsonArray filters = m_opts.value(QStringLiteral("filters")).toArray();
    auto accept = [all, filters](const QBluetoothDeviceInfo& info) {
        if (all) return true;
        for (const auto& fv : filters) {
            const QJsonObject f = fv.toObject();
            if (f.contains(QStringLiteral("name")) && f.value(QStringLiteral("name")).toString() != info.name()) continue;
            if (f.contains(QStringLiteral("namePrefix")) && !info.name().startsWith(f.value(QStringLiteral("namePrefix")).toString())) continue;
            bool svcOk = true;
            for (const auto& s : f.value(QStringLiteral("services")).toArray()) {
                const QString want = s.isDouble() ? QBluetoothUuid(quint16(s.toInt())).toString(QUuid::WithoutBraces) : s.toString();
                bool found = false; for (const auto& u : info.serviceUuids()) found |= u.toString(QUuid::WithoutBraces).compare(want, Qt::CaseInsensitive) == 0;
                svcOk &= found;
            }
            if (svcOk) return true;
        }
        return false;
    };
    connect(agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, [this, accept](const QBluetoothDeviceInfo& info){
        if (!accept(info)) return;
        const QString id = QStringLiteral("bt:") + (info.address().isNull() ? info.deviceUuid().toString(QUuid::WithoutBraces) : info.address().toString());
        for (const auto& d : std::as_const(m_devices)) if (d.value(QStringLiteral("id")).toString() == id) return;
        m_devices << QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), info.name().isEmpty() ? QStringLiteral("Unknown device") : info.name()},
                                 {QStringLiteral("meta"), info.rssi() ? QStringLiteral("%1 dBm").arg(info.rssi()) : QString()}};
        emit devicesChanged();
    });
    connect(agent, &QBluetoothDeviceDiscoveryAgent::finished, this, [this]{ emit finished(QString()); });
    connect(agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this, [this, agent](QBluetoothDeviceDiscoveryAgent::Error){ emit finished(agent->errorString()); });
    agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
#else
    QTimer::singleShot(0, this, [this]{ emit finished(QStringLiteral("Bluetooth support is not built in")); });
#endif
}
