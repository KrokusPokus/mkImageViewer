#ifndef RENAMEDIALOG_H
#define RENAMEDIALOG_H

#include <QDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QString>

class RenameDialog : public QDialog {
    Q_OBJECT

public:
    explicit RenameDialog(const QString& currentFilePath, QWidget* parent = nullptr);
    ~RenameDialog() override = default;

    [[nodiscard]] QString newFilePath() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void setupUi();
    [[nodiscard]] QColor calculateFadedColor(int distance) const;

    QFileInfo m_currentFileInfo;
    QString m_newFilePath;
    QLineEdit* m_nameEdit{nullptr};

    static constexpr int SIBLING_COUNT = 5;
};

#endif // RENAMEDIALOG_H