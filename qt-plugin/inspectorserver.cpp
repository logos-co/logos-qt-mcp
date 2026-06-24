#include "inspectorserver.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QSet>
#include <QWidget>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlContext>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaMethod>
#include <QBuffer>
#include <QPixmap>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QDebug>
#include <QColor>
#include <QUrl>
#include <QRectF>
#include <QPointF>
#include <QSizeF>
#include <QFont>
#include <QFileDialog>

InspectorServer::InspectorServer(QObject *parent)
    : QObject(parent)
{
}

InspectorServer::~InspectorServer()
{
    stop();
}

void InspectorServer::setRootWidget(QWidget *widget)
{
    m_rootWidget = widget;
}

void InspectorServer::start(int port)
{
    if (m_server)
        return;

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &InspectorServer::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, port)) {
        qWarning() << "[QmlInspector] Failed to listen on port" << port
                    << ":" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return;
    }

    qInfo() << "[QmlInspector] Inspector server listening on port"
            << m_server->serverPort();
}

void InspectorServer::stop()
{
    if (m_server) {
        for (auto *client : m_clients)
            client->disconnectFromHost();
        m_clients.clear();
        m_buffers.clear();
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

InspectorServer* InspectorServer::attach(QWidget *widget, int port)
{
    if (port == 0) {
        bool ok;
        int envPort = qEnvironmentVariableIntValue("QML_INSPECTOR_PORT", &ok);
        port = ok ? envPort : 3768;
    }

    auto *inspector = new InspectorServer(widget);
    inspector->setRootWidget(widget);
    inspector->start(port);
    return inspector;
}

// --- Networking ---

void InspectorServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_clients.append(socket);
        m_buffers[socket] = QByteArray();
        connect(socket, &QTcpSocket::readyRead, this, &InspectorServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &InspectorServer::onClientDisconnected);
        qInfo() << "[QmlInspector] Client connected from" << socket->peerAddress().toString();
    }
}

void InspectorServer::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    m_buffers[socket].append(socket->readAll());

    // Process newline-delimited JSON messages
    while (true) {
        int idx = m_buffers[socket].indexOf('\n');
        if (idx < 0) break;

        QByteArray line = m_buffers[socket].left(idx).trimmed();
        m_buffers[socket].remove(0, idx + 1);

        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject errResp;
            errResp["error"] = QString("Parse error: %1").arg(err.errorString());
            sendResponse(socket, errResp);
            continue;
        }

        QJsonObject request = doc.object();
        QJsonObject response = handleCommand(request);
        sendResponse(socket, response);
    }
}

void InspectorServer::onClientDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    m_clients.removeAll(socket);
    m_buffers.remove(socket);
    socket->deleteLater();
    qInfo() << "[QmlInspector] Client disconnected";
}

void InspectorServer::sendResponse(QTcpSocket *socket, const QJsonObject &response)
{
    QByteArray data = QJsonDocument(response).toJson(QJsonDocument::Compact);
    data.append('\n');
    socket->write(data);
    socket->flush();
}

// --- Command dispatch ---

QJsonObject InspectorServer::handleCommand(const QJsonObject &request)
{
    QString command = request.value("command").toString();
    QJsonObject params = request.value("params").toObject();
    int id = request.value("id").toInt(-1);

    QJsonObject result;

    if (command == "getTree")
        result = cmdGetTree(params);
    else if (command == "getProperties")
        result = cmdGetProperties(params);
    else if (command == "setProperty")
        result = cmdSetProperty(params);
    else if (command == "callMethod")
        result = cmdCallMethod(params);
    else if (command == "findByType")
        result = cmdFindByType(params);
    else if (command == "findByProperty")
        result = cmdFindByProperty(params);
    else if (command == "screenshot")
        result = cmdScreenshot(params);
    else if (command == "click")
        result = cmdClick(params);
    else if (command == "sendKeys")
        result = cmdSendKeys(params);
    else if (command == "evaluate")
        result = cmdEvaluate(params);
    else if (command == "findAndClick")
        result = cmdFindAndClick(params);
    else if (command == "listInteractive")
        result = cmdListInteractive(params);
    else if (command == "listFileDialogs")
        result = cmdListFileDialogs(params);
    else if (command == "fileDialogAction")
        result = cmdFileDialogAction(params);
    else
        result = errorResult(QString("Unknown command: %1").arg(command));

    if (id >= 0)
        result["id"] = id;

    return result;
}

// --- Command handlers ---

QJsonObject InspectorServer::cmdGetTree(const QJsonObject &params)
{
    int depth = params.value("depth").toInt(4);
    QString objectId = params.value("objectId").toString();

    QObject *root = nullptr;
    if (objectId.isEmpty()) {
        root = m_rootWidget.data();
    } else {
        root = resolveObject(objectId);
    }

    if (!root)
        return errorResult("Root object not found");

    return okResult({{"tree", serializeObject(root, depth)}});
}

QJsonObject InspectorServer::cmdGetProperties(const QJsonObject &params)
{
    QString objectId = params.value("objectId").toString();
    QObject *obj = resolveObject(objectId);
    if (!obj)
        return errorResult("Object not found: " + objectId);

    QJsonObject data;
    data["objectId"] = objectId;
    data["type"] = QString::fromUtf8(obj->metaObject()->className());
    data["objectName"] = obj->objectName();
    data["properties"] = serializeProperties(obj);

    // List methods/slots
    QJsonArray methods;
    const QMetaObject *meta = obj->metaObject();
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod m = meta->method(i);
        if (m.access() != QMetaMethod::Public) continue;

        QJsonObject mobj;
        mobj["name"] = QString::fromUtf8(m.name());
        mobj["signature"] = QString::fromUtf8(m.methodSignature());
        mobj["returnType"] = QString::fromUtf8(m.typeName());
        switch (m.methodType()) {
            case QMetaMethod::Signal: mobj["kind"] = "signal"; break;
            case QMetaMethod::Slot:   mobj["kind"] = "slot"; break;
            default:                  mobj["kind"] = "method"; break;
        }
        methods.append(mobj);
    }
    data["methods"] = methods;

    return okResult(data);
}

QJsonObject InspectorServer::cmdSetProperty(const QJsonObject &params)
{
    QString objectId = params.value("objectId").toString();
    QString propName = params.value("property").toString();
    QJsonValue jsonVal = params.value("value");

    QObject *obj = resolveObject(objectId);
    if (!obj)
        return errorResult("Object not found: " + objectId);

    // Try meta-property first for proper type conversion
    const QMetaObject *meta = obj->metaObject();
    int propIdx = meta->indexOfProperty(propName.toUtf8().constData());

    QVariant newVal;
    if (propIdx >= 0) {
        QMetaProperty prop = meta->property(propIdx);
        if (!prop.isWritable())
            return errorResult(QString("Property '%1' is not writable").arg(propName));

        // Convert JSON value based on property type
        newVal = jsonVal.toVariant();
        if (!prop.write(obj, newVal))
            return errorResult(QString("Failed to set property '%1'").arg(propName));
    } else {
        // Dynamic property
        obj->setProperty(propName.toUtf8().constData(), jsonVal.toVariant());
    }

    // Read back the value to confirm
    QVariant readBack = obj->property(propName.toUtf8().constData());
    return okResult({
        {"objectId", objectId},
        {"property", propName},
        {"value", variantToJson(readBack)}
    });
}

QJsonObject InspectorServer::cmdCallMethod(const QJsonObject &params)
{
    QString objectId = params.value("objectId").toString();
    QString methodName = params.value("method").toString();
    QJsonArray args = params.value("args").toArray();

    QObject *obj = resolveObject(objectId);
    if (!obj)
        return errorResult("Object not found: " + objectId);

    QVariant returnValue;
    QGenericReturnArgument retArg;
    bool hasReturn = false;

    // Find the method
    const QMetaObject *meta = obj->metaObject();
    int methodIdx = -1;
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod m = meta->method(i);
        if (m.name() == methodName.toUtf8() && m.parameterCount() == args.size()) {
            methodIdx = i;
            break;
        }
    }

    if (methodIdx < 0) {
        // Try invoking by name with no args check (for Q_INVOKABLE)
        bool ok = false;
        if (args.isEmpty()) {
            ok = QMetaObject::invokeMethod(obj, methodName.toUtf8().constData(),
                                            Qt::DirectConnection);
        }
        if (ok) return okResult({{"invoked", methodName}});
        return errorResult(QString("Method '%1' not found with %2 args")
                           .arg(methodName).arg(args.size()));
    }

    // Build variant args list
    QVariantList vargs;
    for (const auto &a : args)
        vargs.append(a.toVariant());

    // Invoke using QMetaMethod with the correct number of arguments
    QMetaMethod method = meta->method(methodIdx);
    bool ok = false;
    switch (vargs.size()) {
    case 0:
        ok = method.invoke(obj, Qt::DirectConnection);
        break;
    case 1:
        ok = method.invoke(obj, Qt::DirectConnection,
                           Q_ARG(QVariant, vargs[0]));
        break;
    case 2:
        ok = method.invoke(obj, Qt::DirectConnection,
                           Q_ARG(QVariant, vargs[0]),
                           Q_ARG(QVariant, vargs[1]));
        break;
    case 3:
        ok = method.invoke(obj, Qt::DirectConnection,
                           Q_ARG(QVariant, vargs[0]),
                           Q_ARG(QVariant, vargs[1]),
                           Q_ARG(QVariant, vargs[2]));
        break;
    case 4:
        ok = method.invoke(obj, Qt::DirectConnection,
                           Q_ARG(QVariant, vargs[0]),
                           Q_ARG(QVariant, vargs[1]),
                           Q_ARG(QVariant, vargs[2]),
                           Q_ARG(QVariant, vargs[3]));
        break;
    default:
        return errorResult(QString("Too many arguments (%1), max 4 supported").arg(vargs.size()));
    }

    if (!ok)
        return errorResult(QString("Failed to invoke '%1'").arg(methodName));

    return okResult({{"invoked", methodName}});
}

QJsonObject InspectorServer::cmdFindByType(const QJsonObject &params)
{
    QString typeName = params.value("typeName").toString();
    if (typeName.isEmpty())
        return errorResult("typeName is required");

    if (!m_rootWidget)
        return errorResult("No root widget");

    QJsonArray matches = findByType(typeName, m_rootWidget.data());
    return okResult({{"matches", matches}, {"count", matches.size()}});
}

QJsonObject InspectorServer::cmdFindByProperty(const QJsonObject &params)
{
    QString propName = params.value("property").toString();
    if (propName.isEmpty())
        return errorResult("property is required");

    QJsonValue matchValue = params.value("value");
    bool hasMatchValue = params.contains("value");

    if (!m_rootWidget)
        return errorResult("No root widget");

    QJsonArray results;
    QList<QObject*> stack;
    QSet<QObject*> visited;
    auto enqueue = [&](QObject *o) {
        if (o && !visited.contains(o)) {
            visited.insert(o);
            stack.append(o);
        }
    };
    enqueue(m_rootWidget.data());

    while (!stack.isEmpty()) {
        QObject *obj = stack.takeFirst();
        if (!obj) continue;

        QVariant val = obj->property(propName.toUtf8().constData());
        if (val.isValid()) {
            bool matches = !hasMatchValue || variantToJson(val) == matchValue;
            // Fallback: compare as strings (handles int/string mismatches like QML text bound to int)
            if (!matches && hasMatchValue && val.canConvert<QString>()) {
                QString valStr = val.toString();
                QString matchStr = matchValue.isDouble()
                    ? QString::number(matchValue.toInt())
                    : matchValue.toString();
                matches = (valStr == matchStr);
            }
            if (matches) {
                QString id = registerObject(obj);
                QJsonObject entry;
                entry["id"] = id;
                entry["type"] = QString::fromUtf8(obj->metaObject()->className());
                entry["objectName"] = obj->objectName();
                entry["value"] = variantToJson(val);
                results.append(entry);
            }
        }

        QQuickItem *item = qobject_cast<QQuickItem*>(obj);
        if (item) {
            for (auto *child : item->childItems())
                enqueue(child);
        }
        QQuickWidget *qw = qobject_cast<QQuickWidget*>(obj);
        if (qw && qw->rootObject())
            enqueue(qw->rootObject());
        for (auto *child : obj->children())
            enqueue(child);
    }

    return okResult({{"matches", results}, {"count", results.size()}});
}

QJsonObject InspectorServer::cmdScreenshot(const QJsonObject &params)
{
    QWidget *target = m_rootWidget.data();
    QString objectId = params.value("objectId").toString();

    if (!objectId.isEmpty()) {
        QObject *obj = resolveObject(objectId);
        if (!obj)
            return errorResult("Object not found: " + objectId);

        QWidget *w = qobject_cast<QWidget*>(obj);
        if (w) target = w;
    }

    if (!target)
        return errorResult("No widget available for screenshot");

    QPixmap pixmap = target->grab();
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    pixmap.save(&buf, "PNG");

    return okResult({
        {"image", QString::fromLatin1(ba.toBase64())},
        {"width", pixmap.width()},
        {"height", pixmap.height()},
        {"format", "png"}
    });
}

QJsonObject InspectorServer::cmdClick(const QJsonObject &params)
{
    if (!m_rootWidget)
        return errorResult("No root widget");

    QWidget *target = m_rootWidget.data();
    double x, y;

    QString objectId = params.value("objectId").toString();
    if (!objectId.isEmpty()) {
        QObject *obj = resolveObject(objectId);
        if (!obj)
            return errorResult("Object not found: " + objectId);

        QWidget *w = qobject_cast<QWidget*>(obj);
        if (w) {
            target = w;
            x = params.contains("x") ? params.value("x").toDouble() : w->width() / 2.0;
            y = params.contains("y") ? params.value("y").toDouble() : w->height() / 2.0;
        } else {
            QQuickItem *item = qobject_cast<QQuickItem*>(obj);
            if (item && item->window()) {
                // Map item center to scene coordinates
                QPointF center(item->width() / 2.0, item->height() / 2.0);
                QPointF scenePos = item->mapToScene(center);
                // Find the QQuickWidget containing this window
                QList<QQuickWidget*> qws = m_rootWidget->findChildren<QQuickWidget*>();
                for (auto *qw : qws) {
                    if (qw->quickWindow() == item->window()) {
                        target = qw;
                        x = scenePos.x();
                        y = scenePos.y();
                        break;
                    }
                }
            } else {
                return errorResult("Cannot click on this object type");
            }
        }
    } else {
        x = params.value("x").toDouble();
        y = params.value("y").toDouble();
    }

    QPointF pos(x, y);
    QPointF globalPos = target->mapToGlobal(pos.toPoint());

    // Find the deepest child widget at these coordinates
    QWidget *child = target->childAt(pos.toPoint());
    if (child) {
        pos = child->mapFrom(target, pos.toPoint());
        globalPos = child->mapToGlobal(pos.toPoint());
        target = child;
    }

    // Events for postEvent must be heap-allocated. Qt will
    // take care of freeing them.
    QMouseEvent *press = new QMouseEvent(
        QEvent::MouseButtonPress, pos, globalPos,
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
    );

    QMouseEvent *release = new QMouseEvent(
        QEvent::MouseButtonRelease, pos, globalPos,
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier
    );

    QApplication::postEvent(target, press);
    QApplication::postEvent(target, release);

    return okResult({{"clicked", true}, {"x", x}, {"y", y},
                     {"widget", QString::fromUtf8(target->metaObject()->className())}});
}

QJsonObject InspectorServer::cmdSendKeys(const QJsonObject &params)
{
    QString text = params.value("text").toString();
    if (text.isEmpty())
        return errorResult("text is required");

    QWidget *target = QApplication::focusWidget();
    if (!target)
        target = m_rootWidget.data();
    if (!target)
        return errorResult("No focus widget");

    for (const QChar &ch : text) {
        int key = ch.unicode();
        Qt::KeyboardModifiers mods = Qt::NoModifier;
        if (ch.isUpper())
            mods = Qt::ShiftModifier;

        QKeyEvent press(QEvent::KeyPress, key, mods, QString(ch));
        QKeyEvent release(QEvent::KeyRelease, key, mods, QString(ch));
        QApplication::sendEvent(target, &press);
        QApplication::sendEvent(target, &release);
    }

    return okResult({{"sent", text}, {"target", QString::fromUtf8(target->metaObject()->className())}});
}

QJsonObject InspectorServer::cmdEvaluate(const QJsonObject &params)
{
    QString expression = params.value("expression").toString();
    if (expression.isEmpty())
        return errorResult("expression is required");

    QQmlEngine *engine = findQmlEngine();
    if (!engine)
        return errorResult("No QML engine found");

    // Find a root QML object to evaluate against
    QObject *contextObj = nullptr;
    QString objectId = params.value("objectId").toString();
    if (!objectId.isEmpty()) {
        contextObj = resolveObject(objectId);
    }

    if (!contextObj) {
        // Find first QQuickWidget and use its root object
        QList<QQuickWidget*> qws = m_rootWidget->findChildren<QQuickWidget*>();
        for (auto *qw : qws) {
            if (qw->rootObject()) {
                contextObj = qw->rootObject();
                break;
            }
        }
    }

    if (!contextObj)
        return errorResult("No QML context object found");

    QQmlContext *context = engine->contextForObject(contextObj);
    if (!context)
        context = engine->rootContext();

    QQmlExpression expr(context, contextObj, expression);
    bool valueIsUndefined = false;
    QVariant result = expr.evaluate(&valueIsUndefined);

    if (expr.hasError())
        return errorResult(QString("Evaluation error: %1").arg(expr.error().toString()));

    return okResult({
        {"result", variantToJson(result)},
        {"undefined", valueIsUndefined}
    });
}

QJsonObject InspectorServer::cmdFindAndClick(const QJsonObject &params)
{
    QString searchText = params.value("text").toString();
    if (searchText.isEmpty())
        return errorResult("text is required");

    QString filterType = params.value("type").toString();
    bool exact = params.value("exact").toBool(false);
    bool visibleOnly = params.value("visibleOnly").toBool(true);

    if (!m_rootWidget)
        return errorResult("No root widget");

    QList<QObject*> stack;
    QSet<QObject*> visited;
    QJsonObject clickResult;
    auto enqueue = [&](QObject *o) {
        if (o && !visited.contains(o)) {
            visited.insert(o);
            stack.append(o);
        }
    };
    enqueue(m_rootWidget.data());

    while (!stack.isEmpty()) {
        QObject *obj = stack.takeFirst();
        if (!obj) continue;

        QQuickItem *item = qobject_cast<QQuickItem*>(obj);
        if (item) {
            for (auto *child : item->childItems())
                enqueue(child);
        }
        QQuickWidget *qw = qobject_cast<QQuickWidget*>(obj);
        if (qw && qw->rootObject())
            enqueue(qw->rootObject());
        for (auto *child : obj->children())
            enqueue(child);

        QVariant textVal = obj->property("text");
        if (!textVal.isValid())
            continue;

        QString text = textVal.toString();
        bool textMatches = exact ? (text == searchText)
                                    : text.contains(searchText, Qt::CaseInsensitive);
        if (!textMatches)
            continue;

        bool typeMatches = true;
        if (!filterType.isEmpty()) {
            QString className = QString::fromUtf8(obj->metaObject()->className());
            typeMatches = (className == filterType || className.contains(filterType));
        }

        if (!typeMatches)
            continue;

        if (visibleOnly) {
            bool visible = obj->property("visible").toBool();
            if (!visible)
                continue;
        }

        QString objectId = registerObject(obj);
        clickResult = cmdClick(QJsonObject{{"objectId", objectId}});
        if (isOk(clickResult)) {
            clickResult["matchedText"] = obj->property("text").toString();
            clickResult["matchedType"] = QString::fromUtf8(obj->metaObject()->className());
            clickResult["matchedId"] = objectId;
            break;
        }
    }

    if (!isOk(clickResult))
        return errorResult(QString("No clickable element found with text '%1'").arg(searchText));

    return clickResult;
}

QJsonObject InspectorServer::cmdListInteractive(const QJsonObject &params)
{
    Q_UNUSED(params);

    if (!m_rootWidget)
        return errorResult("No root widget");

    // Type substrings that indicate interactive elements
    static const QStringList interactiveTypes = {
        "Button", "Delegate", "TextField", "TextInput", "TextEdit",
        "ComboBox", "Slider", "Switch", "CheckBox", "RadioButton",
        "SpinBox", "TabButton", "MenuItem", "MouseArea"
    };

    QJsonArray results;
    QList<QObject*> stack;
    QSet<QObject*> visited;
    auto enqueue = [&](QObject *o) {
        if (o && !visited.contains(o)) {
            visited.insert(o);
            stack.append(o);
        }
    };
    enqueue(m_rootWidget.data());

    while (!stack.isEmpty()) {
        QObject *obj = stack.takeFirst();
        if (!obj) continue;

        QString className = QString::fromUtf8(obj->metaObject()->className());

        // Check if this is an interactive type
        bool isInteractive = false;
        for (const QString &itype : interactiveTypes) {
            if (className.contains(itype, Qt::CaseInsensitive)) {
                isInteractive = true;
                break;
            }
        }

        // Only include interactive elements that are visible
        if (isInteractive) {
            bool isVisible = false;
            QWidget *w = qobject_cast<QWidget*>(obj);
            QQuickItem *qitem = qobject_cast<QQuickItem*>(obj);
            if (w) isVisible = w->isVisible();
            else if (qitem) isVisible = qitem->isVisible();

            if (isVisible) {
                QString id = registerObject(obj);
                QJsonObject entry;
                entry["id"] = id;
                entry["type"] = className;
                entry["objectName"] = obj->objectName();

                QVariant textVal = obj->property("text");
                if (textVal.isValid() && !textVal.toString().isEmpty())
                    entry["text"] = textVal.toString();

                if (w) {
                    entry["enabled"] = w->isEnabled();
                    entry["geometry"] = QJsonObject{
                        {"x", w->x()}, {"y", w->y()},
                        {"width", w->width()}, {"height", w->height()}
                    };
                }
                if (qitem) {
                    entry["enabled"] = qitem->isEnabled();
                    entry["geometry"] = QJsonObject{
                        {"x", qitem->x()}, {"y", qitem->y()},
                        {"width", qitem->width()}, {"height", qitem->height()}
                    };
                }

                results.append(entry);
            }
        }

        // Traverse children
        QQuickItem *item = qobject_cast<QQuickItem*>(obj);
        if (item) {
            for (auto *child : item->childItems())
                enqueue(child);
        }
        QQuickWidget *qw = qobject_cast<QQuickWidget*>(obj);
        if (qw && qw->rootObject())
            enqueue(qw->rootObject());
        for (auto *child : obj->children())
            enqueue(child);
    }

    return okResult({{"elements", results}, {"count", results.size()}});
}

QJsonObject InspectorServer::cmdListFileDialogs(const QJsonObject &params) {
    // There are many more dialog types, but we start with those to make
    // life simpler.
    static const QStringList fileDialogTypes = {
        "QFileDialog", "QQuickFileDialog"
    };

    QWidgetList widgets = QApplication::topLevelWidgets();
    QJsonArray dialogs;
    for (QString dialogType : fileDialogTypes) {
        for (QWidget *rootWidget : widgets) {
            for(auto item : findByType(dialogType, rootWidget)) {
                dialogs.append(item);
            }
        }
    }
    return okResult({{"dialogs", dialogs}});
}

QJsonObject InspectorServer::cmdFileDialogAction(const QJsonObject &params)
{
    QString objectId = params.value("objectId").toString();
    QObject *obj = resolveObject(objectId);
    if (!obj)
        return errorResult("Object not found: " + objectId);

    QFileDialog *fDialog = qobject_cast<QFileDialog*>(obj);
    if (!fDialog && !obj->inherits("QQuickFileDialog")) {
        return errorResult("Object is not a file dialog: " + objectId);
    }

    // Since QQuickFileDialog requires that anyway,
    // just use invokeMethod all over for simplicity.
    QString action = params.value("action").toString();
    if (action == "accept") {
        QMetaObject::invokeMethod(obj, "accept");
    } else if (action == "cancel") {
        QMetaObject::invokeMethod(obj, "reject");
    } else if (action == "select") {
        if (fDialog) {
            fDialog->selectFile(params.value("path").toString());
        } else {
            QString fileUrl = "file://" + params.value("path").toString();
            if (!obj->setProperty("selectedFile", QUrl(fileUrl))) {
                qWarning() << "Failed to set selectedFile property on" << objectId;
            }
        }
    } else if (action == "selectFolder") {
        if (fDialog) {
            fDialog->setDirectory(params.value("path").toString());
        } else {
            QString folderUrl = "file://" + params.value("path").toString();
            if (!obj->setProperty("currentFolder", QUrl(folderUrl))) {
                qWarning() << "Failed to set selectedFolder property on" << objectId;
            }
        }
    } else {
        return errorResult("Unknown action: " + action);
    }

    return okResult();
}

// --- Serialization ---

QJsonObject InspectorServer::serializeObject(QObject *obj, int maxDepth, int currentDepth)
{
    if (!obj)
        return QJsonObject();

    QString id = registerObject(obj);
    QJsonObject result;
    result["id"] = id;
    result["type"] = QString::fromUtf8(obj->metaObject()->className());
    result["objectName"] = obj->objectName();

    // Widget geometry
    QWidget *w = qobject_cast<QWidget*>(obj);
    if (w) {
        result["visible"] = w->isVisible();
        result["enabled"] = w->isEnabled();
        result["geometry"] = QJsonObject{
            {"x", w->x()}, {"y", w->y()},
            {"width", w->width()}, {"height", w->height()}
        };
    }

    // QQuickItem geometry
    QQuickItem *item = qobject_cast<QQuickItem*>(obj);
    if (item) {
        result["visible"] = item->isVisible();
        result["enabled"] = item->isEnabled();
        result["geometry"] = QJsonObject{
            {"x", item->x()}, {"y", item->y()},
            {"width", item->width()}, {"height", item->height()}
        };
        result["opacity"] = item->opacity();
    }

    // QDialog geometry.
    QDialog *dialog = qobject_cast<QDialog*>(obj);
    if (dialog) {
        result["title"] = dialog->windowTitle();
        result["visible"] = dialog->isVisible();
        result["geometry"] = QJsonObject{
            {"x", dialog->x()}, {"y", dialog->y()},
            {"width", dialog->width()}, {"height", dialog->height()}
        };
    }

    // QQuickFileDialog geometry.
    // We need to resort to the metaobject protocol as QQuickDialog has no
    // public C++ API. This is, of course, brittle, and might break with
    // (minor) future versions releases of Qt.
    // FIXME: this is getting pretty long, probably worth it to dismember this
    //   into type-specific and keep the main traversal logic clean.
    if (obj->inherits("QQuickAbstractDialog")) {
        result["title"] = obj->property("title").toString();
        result["visible"] = obj->property("visible").toBool();
        // Geometry is painful cause QML uses its own "real" type - I don't even
        // know if the coordinate system is the same as the rest - so TODO. :-)
    }

    // Include common identifying properties in tree nodes
    static const char* identifyingProps[] = {
        "text", "title", "placeholderText", "source", "currentText", "label", nullptr
    };
    for (int i = 0; identifyingProps[i]; ++i) {
        QVariant val = obj->property(identifyingProps[i]);
        if (val.isValid()) {
            QJsonValue jv = variantToJson(val);
            if (!jv.isNull() && jv != QJsonValue(QString())) {
                result[QString::fromUtf8(identifyingProps[i])] = jv;
            }
        }
    }

    // Children
    if (currentDepth < maxDepth) {
        QJsonArray children;

        // For QQuickWidget, include the QML root item
        QQuickWidget *qw = qobject_cast<QQuickWidget*>(obj);
        if (qw && qw->rootObject()) {
            children.append(serializeObject(qw->rootObject(), maxDepth, currentDepth + 1));
        }

        if (item) {
            // QQuickItem: use childItems() for visual children
            for (QQuickItem *child : item->childItems()) {
                children.append(serializeObject(child, maxDepth, currentDepth + 1));
            }
        } else {
            // QObject/QWidget: use children()
            for (QObject *child : obj->children()) {
                children.append(serializeObject(child, maxDepth, currentDepth + 1));
            }
        }

        result["children"] = children;
        result["childCount"] = children.size();
    } else {
        // Report child count without recursing
        if (item) {
            result["childCount"] = item->childItems().count();
        } else {
            result["childCount"] = obj->children().count();
        }
    }

    return result;
}

QJsonArray InspectorServer::serializeProperties(QObject *obj)
{
    QJsonArray props;
    const QMetaObject *meta = obj->metaObject();

    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QJsonObject p;
        p["name"] = QString::fromUtf8(prop.name());
        p["type"] = QString::fromUtf8(prop.typeName());
        p["value"] = variantToJson(prop.read(obj));
        p["writable"] = prop.isWritable();
        props.append(p);
    }

    // Dynamic properties
    for (const QByteArray &name : obj->dynamicPropertyNames()) {
        QJsonObject p;
        p["name"] = QString::fromUtf8(name);
        p["type"] = "dynamic";
        p["value"] = variantToJson(obj->property(name.constData()));
        p["writable"] = true;
        p["dynamic"] = true;
        props.append(p);
    }

    return props;
}

QJsonValue InspectorServer::variantToJson(const QVariant &var)
{
    if (!var.isValid())
        return QJsonValue::Null;

    switch (var.typeId()) {
    case QMetaType::Bool:
        return var.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
        return var.toLongLong();
    case QMetaType::Float:
    case QMetaType::Double:
        return var.toDouble();
    case QMetaType::QString:
        return var.toString();
    case QMetaType::QUrl:
        return var.toUrl().toString();
    case QMetaType::QColor: {
        QColor c = var.value<QColor>();
        return c.name(QColor::HexArgb);
    }
    case QMetaType::QRect:
    case QMetaType::QRectF: {
        QRectF r = var.toRectF();
        return QJsonObject{{"x", r.x()}, {"y", r.y()},
                           {"width", r.width()}, {"height", r.height()}};
    }
    case QMetaType::QPoint:
    case QMetaType::QPointF: {
        QPointF p = var.toPointF();
        return QJsonObject{{"x", p.x()}, {"y", p.y()}};
    }
    case QMetaType::QSize:
    case QMetaType::QSizeF: {
        QSizeF s = var.toSizeF();
        return QJsonObject{{"width", s.width()}, {"height", s.height()}};
    }
    case QMetaType::QFont: {
        QFont f = var.value<QFont>();
        return QJsonObject{{"family", f.family()}, {"pointSize", f.pointSize()},
                           {"bold", f.bold()}, {"italic", f.italic()}};
    }
    case QMetaType::QStringList: {
        QJsonArray arr;
        for (const auto &s : var.toStringList())
            arr.append(s);
        return arr;
    }
    case QMetaType::QVariantList: {
        QJsonArray arr;
        for (const auto &v : var.toList())
            arr.append(variantToJson(v));
        return arr;
    }
    case QMetaType::QVariantMap: {
        QJsonObject obj;
        auto map = var.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            obj[it.key()] = variantToJson(it.value());
        return obj;
    }
    default:
        // Try toString() as fallback
        if (var.canConvert<QString>())
            return var.toString();
        return QString("<%1>").arg(QString::fromUtf8(var.typeName()));
    }
}

// --- Object registry ---

QString InspectorServer::registerObject(QObject *obj)
{
    if (!obj)
        return QString();

    auto it = m_objToId.find(obj);
    if (it != m_objToId.end()) {
        // Verify the object is still alive
        if (m_idToObj[it.value()].isNull()) {
            // Object was deleted and pointer reused
            m_idToObj.remove(it.value());
            m_objToId.erase(it);
        } else {
            return it.value();
        }
    }

    QString id = QString::number(m_nextId++);
    m_idToObj[id] = obj;
    m_objToId[obj] = id;
    return id;
}

QObject* InspectorServer::resolveObject(const QString &id)
{
    auto it = m_idToObj.find(id);
    if (it == m_idToObj.end())
        return nullptr;

    if (it.value().isNull()) {
        // Object has been deleted
        QString objId = id;
        m_objToId.remove(it.value().data());
        m_idToObj.erase(it);
        return nullptr;
    }

    return it.value().data();
}

void InspectorServer::collectObjects(QObject *root)
{
    if (!root) return;
    registerObject(root);

    QQuickItem *item = qobject_cast<QQuickItem*>(root);
    if (item) {
        for (auto *child : item->childItems())
            collectObjects(child);
    }

    QQuickWidget *qw = qobject_cast<QQuickWidget*>(root);
    if (qw && qw->rootObject())
        collectObjects(qw->rootObject());

    for (auto *child : root->children())
        collectObjects(child);
}

// --- Helpers ---

QQmlEngine* InspectorServer::findQmlEngine()
{
    if (!m_rootWidget)
        return nullptr;

    QList<QQuickWidget*> qws = m_rootWidget->findChildren<QQuickWidget*>();
    for (auto *qw : qws) {
        if (qw->engine())
            return qw->engine();
    }
    return nullptr;
}

QWidget* InspectorServer::findWidgetAt(int x, int y)
{
    if (!m_rootWidget) return nullptr;
    QWidget *child = m_rootWidget->childAt(x, y);
    return child ? child : m_rootWidget.data();
}

bool InspectorServer::isError(const QJsonObject &obj)
{
    return obj.contains("error");
}

bool InspectorServer::isOk(const QJsonObject &obj)
{
    return obj.contains("ok");
}

QJsonObject InspectorServer::errorResult(const QString &msg)
{
    return QJsonObject{{"error", msg}};
}

QJsonObject InspectorServer::okResult(const QJsonObject &data)
{
    QJsonObject result = data;
    result["ok"] = true;
    return result;
}

QJsonArray InspectorServer::findByType(const QString &typeName, QWidget *root)
{
    // Collect all objects and filter by type
    QJsonArray results;
    QSet<QObject*> visited;
    QList<QObject*> stack;
    // cost proportional to tree size, not parent-chain multiplicity.
    auto enqueue = [&](QObject *o) {
        if (o && !visited.contains(o)) {
            visited.insert(o);
            stack.append(o);
        }
    };

    stack.append(root);

    while (!stack.isEmpty()) {
        QObject *obj = stack.takeFirst();
        if (!obj) continue;

        QString className = QString::fromUtf8(obj->metaObject()->className());
        // Match exact class name or a part of it (e.g., "Button" matches "QQuickButton" or
        // "Button_QML_TYPE_XX")
        if (className == typeName || className.contains(typeName)) {
            results.append(serializeObject(obj, 0, 0));
        }

        QQuickItem *item = qobject_cast<QQuickItem*>(obj);
        if (item) {
            for (auto *child : item->childItems())
                enqueue(child);
        }
        QQuickWidget *qw = qobject_cast<QQuickWidget*>(obj);
        if (qw && qw->rootObject())
            enqueue(qw->rootObject());
        for (auto *child : obj->children())
            enqueue(child);
    }

    return results;
}
