#include "MultiMonitorSupport.h"
#include "SettingsProvider.h"

#include <QGuiApplication>
#include <QScreen>
#include <QApplication>
#include <QStyle>

static QRect defaultWindowRect = QRect(0, 0, 0, 0);

// returns true if the system is a multi monitor system
bool CMultiMonitorSupport::IsMultiMonitorSystem() {
    // Prüft, ob mehr als ein Monitor im System registriert ist
    return QGuiApplication::screens().size() > 1;
}

// virtual desktop (spanning all monitors)
QRect CMultiMonitorSupport::GetVirtualDesktop() {
    if (QScreen* primaryScreen = QGuiApplication::primaryScreen()) {
        // virtualGeometry() gibt das Rechteck zurück, das alle Monitore im Verbund abspannt
        return primaryScreen->virtualGeometry();
    }
    
    // Fallback, falls kein Screen verfügbar ist
    return QRect(0, 0, 1920, 1080);
}

// Display rectangle of the monitor, use -1 for largest monitor, 0 for primary monitor or 
// 1 to n for the secondary or other monitors
QRect CMultiMonitorSupport::GetMonitorRect(int nIndex) {
    const auto screens = QGuiApplication::screens();
    
    // Fallback auf primären Bildschirm oder Standardauflösung, wenn keine Screens da sind
    if (screens.isEmpty()) {
        QScreen* primary = QGuiApplication::primaryScreen();
        return primary ? primary->geometry() : QRect(0, 0, 1920, 1080);
    }

    // Fall 1: Primärmonitor (Index 0)
    if (nIndex == 0) {
        QScreen* primary = QGuiApplication::primaryScreen();
        return primary ? primary->geometry() : screens.first()->geometry();
    }

    // Fall 2: Spezifischer Monitor nach Index (1 bis n)
    if (nIndex > 0 && nIndex < screens.size()) {
        return screens.at(nIndex)->geometry();
    }

    // Fall 3: Grösster Monitor (-1 oder ungültiger Index)
    if (nIndex == -1) {
        QScreen* largestScreen = screens.first();
        int maxArea = largestScreen->geometry().width() * largestScreen->geometry().height();

        for (int i = 1; i < screens.size(); ++i) {
            QRect geo = screens.at(i)->geometry();
            int area = geo.width() * geo.height();
            if (area > maxArea) {
                maxArea = area;
                largestScreen = screens.at(i);
            }
        }
        return largestScreen->geometry();
    }

    // Fallback auf den Primärmonitor, falls der Index völlig out of bounds ist
    QScreen* primary = QGuiApplication::primaryScreen();
    return primary ? primary->geometry() : screens.first()->geometry();
}

// Display rectangle of monitor the main part of the given window is shown on
QRect CMultiMonitorSupport::GetMonitorRect(const QWidget* pWidget) {
    QScreen* screen = nullptr;
    
    // Bildschirm über das Widget ermitteln
    if (pWidget != nullptr) {
        screen = pWidget->screen();
    }
    
    // Fallback auf den Primärschirm, falls das Widget noch keinen Screen hat
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    
    if (screen != nullptr) {
        return screen->geometry();
    }
    
    // Absoluter Notfall-Fallback, falls gar kein Screen greifbar ist
    return QRect(0, 0, 1920, 1080);
}

// Gets the working rectangle (monitor rectangle excluding taskbar) of the monitor the given window is displayed on
QRect CMultiMonitorSupport::GetWorkingRect(const QWidget* pWidget) {
    QScreen* screen = nullptr;
    
    // Bildschirm über das Widget ermitteln
    if (pWidget != nullptr) {
        screen = pWidget->screen();
    }
    
    // Fallback auf den Primärschirm, falls das Widget noch keinen Screen zugewiesen hat
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    
    if (screen != nullptr) {
        // availableGeometry() entspricht exakt dem Win32 rcWork (ohne Taskleiste)
        return screen->availableGeometry();
    }
    
    // Absoluter Fallback
    return QRect(0, 0, 1920, 1080);
}

// Gets the default window rectangle for windowed mode
QRect CMultiMonitorSupport::GetDefaultWindowRect() {
    CSettingsProvider& settings = CSettingsProvider::This();
    
    // Ermitteln, welches Basis-Rechteck geladen werden soll
    QRect windowRect = !defaultWindowRect.isEmpty() ? defaultWindowRect : (settings.StickyWindowSize() ? settings.StickyWindowRect() : settings.DefaultWindowRect());
    
    QRect rectAllScreens = CMultiMonitorSupport::GetVirtualDesktop();
    
    // If we still have no valid windowRect OR if the windowRect we have is outside the screen rect, reset to a calculated default
    if (windowRect.isEmpty() || !rectAllScreens.intersects(windowRect)) {
        QRect monitorRect;
        const auto screens = QGuiApplication::screens();
        int displayIndex = settings.DisplayMonitor();
        
        if (displayIndex >= 0 && displayIndex < screens.size()) {
            monitorRect = screens.at(displayIndex)->geometry();
        } else {
            QScreen* primary = QGuiApplication::primaryScreen();
            monitorRect = primary ? primary->geometry() : QRect(0, 0, 1920, 1080);
        }

        int nDesiredWidth = monitorRect.width() * 2 / 3;
        int nDesiredHeight = nDesiredWidth * 3 / 4;
        
        QSize borderSize = GetTotalBorderSize();
        nDesiredWidth += borderSize.width();
        nDesiredHeight += borderSize.height();
        
        windowRect = QRect(
            QPoint(monitorRect.left() + (monitorRect.width() - nDesiredWidth) / 2, 
                   monitorRect.top() + (monitorRect.height() - nDesiredHeight) / 2), 
            QSize(nDesiredWidth, nDesiredHeight)
        );
    }
    
    return windowRect;
}

// Sets the default window rectangle for windowed mode
void CMultiMonitorSupport::SetDefaultWindowRect(QRect rect) {
    defaultWindowRect = rect;
}

// Gets the default client rectangle for windowed mode
QRect CMultiMonitorSupport::GetDefaultClientRectInWindowMode() {
    QRect wndRect = CMultiMonitorSupport::GetDefaultWindowRect();
    QSize borderSize = GetTotalBorderSize();
    
    return QRect(
        0, 0, 
        wndRect.width() - borderSize.width(), 
        wndRect.height() - borderSize.height()
    );
}

QSize CMultiMonitorSupport::GetTotalBorderSize() {
    QStyle* style = QApplication::style();
    
    // Standard-Rahmenbreite (entspricht in etwa SM_CXSIZEFRAME)
    int frameWidth = style->pixelMetric(QStyle::PM_DefaultFrameWidth);
    
    // Titelleistenhöhe (entspricht SM_CYCAPTION)
    int titleBarHeight = style->pixelMetric(QStyle::PM_TitleBarHeight);
    
    // Falls PM_TitleBarHeight auf manchen Plattformen/Styles 0 liefert, 
    // setzen wir einen sinnvollen Fallback (z.B. Font-Metriken oder Standardwert)
    if (titleBarHeight <= 0) {
        titleBarHeight = 25; // Typischer Standardwert
    }

    // Da der Rahmen links und rechts bzw. oben und unten existiert, * 2 für die Breite
    int nBorderWidth = frameWidth * 2;
    // Gesamthöhe setzt sich aus Titelleiste + oberem/unteren Rahmen zusammen
    int nBorderHeight = titleBarHeight + (frameWidth * 2);

    return QSize(nBorderWidth, nBorderHeight);
}
