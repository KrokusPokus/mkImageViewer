#include "RenameDialog.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QStringList>
#include <QtMath>

#include <algorithm>

namespace {

// NAME_MAX unter Linux: maximale Laenge EINES Dateinamens in Bytes (nicht Zeichen!).
constexpr int kMaxNameBytes = 255;

// Nachbarnamen werden auf diese Breite begrenzt, sonst macht ein 200 Zeichen langer Dateiname
// den ganzen Dialog beliebig breit (adjustSize() kennt kein Maximum).
constexpr int kMaxLabelWidth = 360;

// Trennt "archive.tar.gz" in "archive.tar" + ".gz". Ein fuehrender Punkt zaehlt nicht als
// Endung: ".gitignore" bleibt komplett Name (QFileInfo::suffix() wuerde hier "gitignore" liefern
// und den Basisnamen leer lassen, das Eingabefeld waere dann leer).
void splitName(const QString& fileName, QString& base, QString& ext) {
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        base = fileName.left(dot);
        ext = fileName.mid(dot);
    } else {
        base = fileName;
        ext.clear();
    }
}

// Einzeiliges Label, das zu lange Namen in der Mitte mit "..." kuerzt statt den Dialog aufzuweiten.
class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;

    QSize sizeHint() const override {
        QSize s = QLabel::sizeHint();
        s.setWidth(qMin(s.width(), kMaxLabelWidth));
        return s;
    }
    QSize minimumSizeHint() const override {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setPen(palette().color(foregroundRole()));
        const QString t = fontMetrics().elidedText(text(), Qt::ElideMiddle, width());
        p.drawText(rect(), int(alignment()) | Qt::TextSingleLine, t);
    }
};

} // namespace

RenameDialog::RenameDialog(const QString& currentFilePath, QWidget* parent)
    // WindowStaysOnTopHint wie bisher beibehalten (gedacht fuer den Vollbildmodus des Viewers).
    : QDialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint),
      // absoluter Pfad: sonst waere auch der neue Pfad relativ zum Arbeitsverzeichnis
      m_currentFileInfo(QFileInfo(currentFilePath).absoluteFilePath()) {

    setWindowTitle(tr("Rename file"));
    setMinimumWidth(420);
    splitName(m_currentFileInfo.fileName(), m_baseName, m_extension);
    setupUi();
}

QString RenameDialog::newFilePath() const {
    return m_newFilePath;
}

// Farbe zwischen Hintergrund- und Textfarbe, je weiter weg von der aktuellen Datei, desto blasser
// (0.6 ^ |distance|). Ueber die Palette statt per Stylesheet, damit sie dem Theme folgt.
void RenameDialog::applyFade(QWidget* widget, int distance) {
    const QColor bg = widget->palette().color(QPalette::Window);
    const QColor fg = widget->palette().color(QPalette::WindowText);
    const double f = qPow(0.6, qAbs(distance));

    const QColor c(qRound(bg.red()   + (fg.red()   - bg.red())   * f),
                   qRound(bg.green() + (fg.green() - bg.green()) * f),
                   qRound(bg.blue()  + (fg.blue()  - bg.blue())  * f));

    QPalette p = widget->palette();
    p.setColor(QPalette::WindowText, c);
    widget->setPalette(p);
}

void RenameDialog::setupUi() {
    // Ein gemeinsames Grid: Spalte 0 = Namen (rechtsbuendig), Spalte 1 = Endungen (linksbuendig).
    // Mit einem eigenen QHBoxLayout je Zeile bekam die Zeile mit dem QLineEdit (Expanding) eine
    // andere Aufteilung als die Label-Zeilen, die Endungen standen dadurch nicht untereinander.
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(20, 20, 20, 20);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(0, 1);

    // Dateien im Ordner. Hidden gehoert dazu, sonst fehlt eine versteckte aktuelle Datei in der
    // Liste. NoSort: der QDir-Standard sortiert selbst, wir sortieren gleich nochmal logisch.
    const QDir dir = m_currentFileInfo.absoluteDir();
    const QString currentName = m_currentFileInfo.fileName();
    QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);

    // Die aktuelle Datei muss immer vorkommen (z.B. defekter Symlink wird von QDir::Files nicht
    // gelistet) - sonst bliebe der Dialog ohne Eingabefeld und damit unbenutzbar.
    if (!names.contains(currentName)) {
        names.append(currentName);
    }

    // Logische Sortierung (1, 2, ..., 10 statt 1, 10, 2). Bei Gleichstand nach dem Rohtext, damit
    // der Vergleich eine strikte Ordnung bleibt (Namen, die sich nur in Gross/Klein unterscheiden).
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&collator](const QString& a, const QString& b) {
        const int c = collator.compare(a, b);
        return c != 0 ? c < 0 : a < b;
    });

    const int targetIndex = static_cast<int>(names.indexOf(currentName));
    const int startIndex = std::max(0, targetIndex - SIBLING_COUNT);
    const int endIndex = std::min(static_cast<int>(names.size()) - 1, targetIndex + SIBLING_COUNT);

    int row = 0;
    for (int i = startIndex; i <= endIndex; ++i) {
        const int distance = i - targetIndex;

        if (distance == 0) {
            // Leerzeile davor (ersetzt die alte Abstands-Feinjustierung "y+11")
            grid->setRowMinimumHeight(row++, 6);

            m_nameEdit = new QLineEdit(m_baseName, this);
            m_nameEdit->setAlignment(Qt::AlignRight);
            m_nameEdit->setMinimumWidth(240);
            connect(m_nameEdit, &QLineEdit::textChanged, this, &RenameDialog::onTextChanged);
            connect(m_nameEdit, &QLineEdit::returnPressed, this, &RenameDialog::commitRename);

            auto* extLabel = new QLabel(m_extension, this);
            extLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

            grid->addWidget(m_nameEdit, row, 0);
            grid->addWidget(extLabel, row, 1);
            ++row;

            grid->setRowMinimumHeight(row++, 6);
        } else {
            QString base, ext;
            splitName(names.at(i), base, ext);

            auto* nameLabel = new ElidedLabel(base, this);
            nameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            applyFade(nameLabel, distance);

            auto* extLabel = new QLabel(ext, this);
            extLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            applyFade(extLabel, distance);

            grid->addWidget(nameLabel, row, 0);
            grid->addWidget(extLabel, row, 1);
            ++row;
        }
    }

    m_nameEdit->selectAll();
    m_nameEdit->setFocus();
    adjustSize();
}

// Prueft einen eingegebenen Basisnamen (ohne Endung). Linux-Regeln: verboten sind nur '/' und
// das Nullzeichen; "." und ".." sind reservierte Namen; die Laenge ist auf 255 BYTES begrenzt.
RenameDialog::Check RenameDialog::checkName(const QString& rawBaseName) const {
    // Wie bisher: umgebende Leerzeichen werden verworfen (meist ein Tippfehler).
    const QString base = rawBaseName.trimmed();

    if (base == m_baseName) {
        return { NameState::Unchanged, QString() };
    }
    if (base.isEmpty()) {
        return { NameState::Invalid, tr("The name must not be empty.") };
    }
    // Ohne diese Pruefung wuerde "a/b" die Datei in einen anderen Ordner verschieben und
    // "../x" aus dem Ordner herausfuehren.
    if (base.contains(QLatin1Char('/')) || base.contains(QChar(u'\0'))) {
        return { NameState::Invalid, tr("The name must not contain '/'.") };
    }

    const QString fullName = base + m_extension;
    if (fullName == QLatin1String(".") || fullName == QLatin1String("..")) {
        return { NameState::Invalid, tr("This name is reserved.") };
    }
    if (fullName.toUtf8().size() > kMaxNameBytes) {
        return { NameState::Invalid, tr("The name is too long.") };
    }

    // Kollision. isSymLink() zusaetzlich, weil exists() einem defekten Symlink folgt und dann
    // "false" meldet, der Name aber trotzdem belegt ist.
    const QFileInfo target(m_currentFileInfo.absoluteDir().filePath(fullName));
    if (target.exists() || target.isSymLink()) {
        return { NameState::Collision, tr("A file with this name already exists.") };
    }

    return { NameState::Ok, QString() };
}

void RenameDialog::onTextChanged() {
    const Check check = checkName(m_nameEdit->text());
    m_state = check.state;

    const bool bad = (check.state == NameState::Invalid || check.state == NameState::Collision);
    // Stylesheet statt Palette: wird von allen Styles zuverlaessig beachtet.
    m_nameEdit->setStyleSheet(bad
        ? QStringLiteral("QLineEdit { background-color: #c62828; color: white;"
                         " selection-background-color: #ffcdd2; selection-color: black; }")
        : QString());
    m_nameEdit->setToolTip(bad ? check.reason : QString());
}

void RenameDialog::commitRename() {
    // Name unveraendert: nichts zu tun, Dialog schliessen.
    if (m_state == NameState::Unchanged) {
        reject();
        return;
    }

    // Erneut pruefen: seit der letzten Eingabe kann die Datei inzwischen angelegt worden sein.
    onTextChanged();
    if (m_state != NameState::Ok) {
        return;   // rot markiert: Return wird ignoriert
    }

    const QString newPath = m_currentFileInfo.absoluteDir().filePath(m_nameEdit->text().trimmed() + m_extension);

    // QFile::rename ueberschreibt ein vorhandenes Ziel nie (im Gegensatz zu std::filesystem::rename
    // bzw. rename(2)), auch nicht, wenn es erst nach der Pruefung oben entstanden ist.
    QFile file(m_currentFileInfo.absoluteFilePath());
    if (!file.rename(newPath)) {
        QMessageBox::critical(this, tr("Rename failed"), file.errorString());
        onTextChanged();   // Zustand neu bewerten; Dialog bleibt offen
        return;
    }

    m_newFilePath = newPath;
    accept();
}
