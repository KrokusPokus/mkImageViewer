#ifndef MULTIMONITORSUPPORT_H
#define MULTIMONITORSUPPORT_H

#include <QRect>


class QWidget; // Forward-Deklaration

// Support for multiple-monitor systems (Qt-based)
class CMultiMonitorSupport
{
public:
    // returns true if the system is a multi monitor system
    static bool IsMultiMonitorSystem();

    // virtual desktop (spanning all monitors)
    static QRect GetVirtualDesktop();

    // Display rectangle of the monitor, use -1 for largest monitor, 0 for primary monitor or 
    // 1 to n for the secondary or other monitors
    static QRect GetMonitorRect(int nIndex);

    // Display rectangle of monitor the main part of the given window is shown on
    static QRect GetMonitorRect(const QWidget* pWidget);

    // Gets the working rectangle (monitor rectangle excluding taskbar) of the monitor the given window is displayed on
    static QRect GetWorkingRect(const QWidget* pWidget);

    // Gets the default window rectangle for windowed mode
    static QRect GetDefaultWindowRect();

    // Sets the default window rectangle for windowed mode
    static void SetDefaultWindowRect(QRect rect);
    
private:
    CMultiMonitorSupport() = delete; // Statische Klasse nicht instanziierbar
};

#endif // MULTIMONITORSUPPORT_H