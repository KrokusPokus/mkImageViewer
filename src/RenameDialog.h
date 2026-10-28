#ifndef RENAMEDIALOG_H
#define RENAMEDIALOG_H

#include <QColor>
#include <QDialog>
#include <QFileInfo>
#include <QString>

class QLineEdit;
class QWidget;

// Dialog zum Umbenennen einer Datei (nur Linux). Zeigt die alphabetischen Nachbarn im selben Ordner
// ausgegraut an, der Name der aktuellen Datei ist editierbar, die Endung bleibt fest.
//
// Der Dialog fuehrt das Umbenennen selbst durch. Gelingt es nicht, bleibt er offen. Der Name wird
// schon waehrend der Eingabe geprueft: ungueltige Namen und Kollisionen faerben das Eingabefeld rot
// (weisser Text), Return/Enter wird dann ignoriert.
class RenameDialog : public QDialog {
    Q_OBJECT

public:
    explicit RenameDialog(const QString& currentFilePath, QWidget* parent = nullptr);
    ~RenameDialog() override = default;

    // Neuer absoluter Pfad. Nur nach erfolgreichem Umbenennen (exec() == Accepted) gesetzt, sonst leer.
    [[nodiscard]] QString newFilePath() const;

private slots:
    void onTextChanged();   // Live-Validierung bei jeder Eingabe
    void commitRename();    // Return/Enter im Eingabefeld

private:
    enum class NameState {
        Unchanged,   // Name gleich dem bisherigen: Return schliesst den Dialog ohne Aenderung
        Ok,          // gueltig und frei
        Invalid,     // leer, '/', zu lang, "." oder ".."
        Collision    // Ziel existiert bereits
    };
    struct Check {
        NameState state;
        QString reason;   // nur bei Invalid/Collision, wird als Tooltip angezeigt
    };

    void setupUi();
    [[nodiscard]] Check checkName(const QString& rawBaseName) const;
    static void applyFade(QWidget* widget, int distance);

    QFileInfo m_currentFileInfo;   // immer mit absolutem Pfad
    QString m_baseName;            // Name ohne Endung (der editierbare Teil)
    QString m_extension;           // inkl. Punkt, z.B. ".jpg"; leer wenn keine
    QString m_newFilePath;
    QLineEdit* m_nameEdit{nullptr};
    NameState m_state{NameState::Unchanged};

    static constexpr int SIBLING_COUNT = 5;
};

#endif // RENAMEDIALOG_H
