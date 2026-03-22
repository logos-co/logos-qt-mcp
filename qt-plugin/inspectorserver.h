#ifndef INSPECTORSERVER_H
#define INSPECTORSERVER_H

#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QMap>
#include <QList>

class QTcpServer;
class QTcpSocket;
class QWidget;
class QQuickItem;
class QQmlEngine;

class InspectorServer : public QObject
{
    Q_OBJECT

public:
    explicit InspectorServer(QObject *parent = nullptr);
    ~InspectorServer();

    void setRootWidget(QWidget *widget);
    void start(int port);
    void stop();

    /// One-liner: attach to a widget and start on the given port.
    /// If port == 0, reads QML_INSPECTOR_PORT env var (default 3768).
    static InspectorServer* attach(QWidget *widget, int port = 0);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();

private:
    // Command dispatch
    QJsonObject handleCommand(const QJsonObject &request);

    // Command handlers
    QJsonObject cmdGetTree(const QJsonObject &params);
    QJsonObject cmdGetProperties(const QJsonObject &params);
    QJsonObject cmdSetProperty(const QJsonObject &params);
    QJsonObject cmdCallMethod(const QJsonObject &params);
    QJsonObject cmdFindByType(const QJsonObject &params);
    QJsonObject cmdFindByProperty(const QJsonObject &params);
    QJsonObject cmdScreenshot(const QJsonObject &params);
    QJsonObject cmdClick(const QJsonObject &params);
    QJsonObject cmdSendKeys(const QJsonObject &params);
    QJsonObject cmdEvaluate(const QJsonObject &params);
    QJsonObject cmdFindAndClick(const QJsonObject &params);
    QJsonObject cmdListInteractive(const QJsonObject &params);

    // Tree serialization
    QJsonObject serializeObject(QObject *obj, int maxDepth, int currentDepth = 0);
    QJsonArray serializeProperties(QObject *obj);
    QJsonValue variantToJson(const QVariant &var);

    // Object registry
    QString registerObject(QObject *obj);
    QObject* resolveObject(const QString &id);
    void collectObjects(QObject *root);

    // Helpers
    QQmlEngine* findQmlEngine();
    QWidget* findWidgetAt(int x, int y);
    QJsonObject errorResult(const QString &msg);
    QJsonObject okResult(const QJsonObject &data = QJsonObject());

    void sendResponse(QTcpSocket *socket, const QJsonObject &response);

    QTcpServer *m_server = nullptr;
    QList<QTcpSocket*> m_clients;
    QMap<QTcpSocket*, QByteArray> m_buffers;

    QPointer<QWidget> m_rootWidget;

    QMap<QString, QPointer<QObject>> m_idToObj;
    QMap<QObject*, QString> m_objToId;
    int m_nextId = 1;
};

#endif // INSPECTORSERVER_H
