// The app the inspector's tests drive: popup.qml in a QQuickWidget, hosted the way Basecamp hosts its views.
#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "inspectorserver.h"

// Stands in for QDialog::exec or a blocking IPC call: a click handler that nests an event loop.
class NestedLoop : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    bool running() const { return m_loop != nullptr; }

    Q_INVOKABLE void run()
    {
        QEventLoop loop;
        m_loop = &loop;
        emit runningChanged();
        loop.exec();
        m_loop = nullptr;
        emit runningChanged();
    }

    Q_INVOKABLE void quit()
    {
        if (m_loop)
            m_loop->quit();
    }

signals:
    void runningChanged();

private:
    QEventLoop *m_loop = nullptr;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QWidget window;
    NestedLoop nestedLoop;
    auto *view = new QQuickWidget(&window);
    view->rootContext()->setContextProperty("nestedLoop", &nestedLoop);
    view->setSource(QUrl("qrc:/popup.qml"));
    if (view->status() != QQuickWidget::Ready) {
        for (const auto &error : view->errors())
            qCritical() << error;
        return 1;
    }
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
    window.resize(480, 320);
    window.show();

    InspectorServer::attach(&window);
    return app.exec();
}

#include "main.moc"
