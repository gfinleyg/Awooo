// antnative.dll in a Qt Widgets process, and the stick list over it (src/ant/AntSticks)

#include <QtTest>
#include <QWidget>

#include <windows.h>

#include <antnative.hpp>

#include "AntSticks.hpp"

namespace {

// The C++ runtime DLL this process loaded, and its file version as major * 100 + minor (14.40 -> 1440); 0 if unknown
int loadedRuntimeVersion() {
#ifdef _DEBUG
    const wchar_t* runtime = L"msvcp140d.dll";
#else
    const wchar_t* runtime = L"msvcp140.dll";
#endif
    HMODULE module = GetModuleHandleW(runtime);
    if (module == nullptr) {
        return 0;
    }
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return 0;
    }
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) {
        return 0;
    }
    QByteArray info(static_cast<qsizetype>(size), Qt::Uninitialized);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (!GetFileVersionInfoW(path, 0, size, info.data())
        || !VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) || fixed == nullptr) {
        return 0;
    }
    return HIWORD(fixed->dwFileVersionMS) * 100 + LOWORD(fixed->dwFileVersionMS);
}

}  // namespace

class TestAntSticks : public QObject {
    Q_OBJECT

private slots:
    void dllMatchesHeader() {
        // the antnative.dll loaded is the one antnative.h describes
        QCOMPARE(ant_api_version(), ANT_API_VERSION);
    }

    void runtimeNewEnoughForAntNative() {
        // What crashed the Python app: PyQt6's msvcp140.dll (14.26) loaded ahead of the newer one AntNative's
        // std::mutex needs. A runtime at least as new as this compiler (14.40 for _MSC_VER 1940) must be the one loaded.
        QWidget widget;
        widget.show();
        const int required = 1400 + (_MSC_VER - 1900);
        const int loaded = loadedRuntimeVersion();
        QVERIFY2(loaded >= required, qPrintable(QStringLiteral("C++ runtime %1.%2 loaded, %3.%4 or newer needed")
                                                    .arg(loaded / 100).arg(loaded % 100)
                                                    .arg(required / 100).arg(required % 100)));
    }

    void listsSticksBesideQtWidgets() {
        // the USB list (and antnative's locks) used with Qt Widgets loaded: every stick antnative sees is listed
        QWidget widget;
        widget.show();
        const QList<awo::ant::Stick> sticks = awo::ant::attachedSticks();
        QCOMPARE(sticks.size(), static_cast<qsizetype>(antnative::list_devices().size()));
        for (qsizetype i = 0; i < sticks.size(); ++i) {
            QCOMPARE(sticks[i].index, static_cast<std::uint32_t>(i));
        }
    }

    void describesSticks() {
        awo::ant::Stick stick;
        stick.index = 1;
        stick.serialNumber = 1234567;
        stick.product = QStringLiteral("ANT USBStick2");
        QCOMPARE(describe(stick), QStringLiteral("1: ANT USBStick2 (serial 1234567)"));
        stick.product.clear();
        QCOMPARE(describe(stick), QStringLiteral("1: ANT USB stick (serial 1234567)"));
    }

    void identitySurvivesReplug() {
        // a stick plugged back in at another index is still the same stick
        awo::ant::Stick before;
        before.index = 0;
        before.vid = 0x0FCF;
        before.pid = 0x1008;
        before.serialNumber = 42;
        awo::ant::Stick after = before;
        after.index = 1;
        QVERIFY(before.identity() == after.identity());
        after.serialNumber = 43;
        QVERIFY(before.identity() != after.identity());
    }
};

QTEST_MAIN(TestAntSticks)
#include "tst_ant_sticks.moc"
