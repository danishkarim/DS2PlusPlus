#include <QTextStream>
#include <QDebug>

#include <QUuid>

#include <QDir>
#include <QDirIterator>

#include <QSqlQuery>
#include <QSqlTableModel>
#include <QSqlRecord>
#include <QSqlError>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

#include <QSharedPointer>

#include <QCommandLineParser>

#include <QSerialPort>

#include "exception.h"
#include "manager.h"

void waitForThisManyBytes(QSerialPort &aPort, qint64 someNumberOfBytes)
{
    while ((!aPort.waitForReadyRead(250)) or (aPort.bytesAvailable() < someNumberOfBytes)) {
        //qDebug() << "Bytes available: " << aPort.bytesAvailable() << " / Ready to read: " << aPort.waitForReadyRead(250);
    }
}

namespace DS2PlusPlus {
    const QString Manager::DPP_DIR = QString("~/.dpp");
    const QString Manager::DPP_DB_PATH = QString("dppdb.sqlite3");
    const QString Manager::DPP_JSON_PATH = QString("json");

    QTextStream qOut(stdout);
    QTextStream qErr(stderr);

    QString expandTilde(const QString &path)
    {
        QString ret(path);
        if (ret.startsWith ("~/")) {
            ret.replace (0, 1, QDir::homePath());
        }

        return ret;
    }

    Manager::Manager(QSharedPointer<QCommandLineParser> aParser, QObject *parent) :
        QObject(parent), _dppDir(QString::null), _serialPort(new QSerialPort), _cliParser(aParser)
    {
        if (!_cliParser.isNull()) {
            // We should really check to see if the user has already defined this, and if so we should whine.
            QCommandLineOption targetDirectoryOption(QStringList() << "f" << "port",
                      "Read from serial port <port>.",
                      "port");
            aParser->addOption(targetDirectoryOption);
        } else {
            initializeManager();
        }
    }

    void Manager::initializeManager()
    {
        if (_serialPortPath.isEmpty()) {
            _serialPortPath = getenv("DPP_PORT");
        }

        if (!_cliParser.isNull() and _serialPortPath.isEmpty()) {
            _serialPortPath = _cliParser->value("port");
        }

        // Path of last resort.
        if (_serialPortPath == "auto") {
#ifdef __APPLE__
            _serialPortPath = "/dev/tty.usbserial";
#elif _WIN32
            _serialPortPath = "COM3:";
#else
            _serialPortPath = "/dev/tty.usbserial";
#endif
        }

        // Check to make sure our directory
        // So we can have multiple connections...
        QString connName(QUuid::createUuid().toString());
        _db = QSqlDatabase::addDatabase("QSQLITE", connName);

        QString dppDbPath = QString("%1%2%3").arg(dppDir()).arg(QDir::separator()).arg(DPP_DB_PATH);
        _db.setDatabaseName(expandTilde(dppDbPath));

        if (!_db.open()) {
            qErr << "Couldn't open the database: " << _db.lastError().databaseText() << endl;
            throw "Couldn't open the database";
        }

        // Determine serial port -- platform specific
        if (!_serialPortPath.isEmpty()) {
            _serialPort->setPortName(_serialPortPath);

            // Configure it
            _serialPort->setBaudRate(QSerialPort::Baud9600);
            _serialPort->setDataBits(QSerialPort::Data8);
            _serialPort->setParity(QSerialPort::EvenParity);
            _serialPort->setStopBits(QSerialPort::OneStop);

            // Open it
            if (_serialPort->isOpen()) {
                _serialPort->close();
            }

            if (!_serialPort->open(QIODevice::ReadWrite)) {
                throw new SerialIOError(QString("'%1': %2").arg(_serialPort->portName()).arg(_serialPort->errorString()));
            }
        }

    }

    QSqlTableModel *Manager::modulesTable() {
        if (!_db.isOpen()) {
            qDebug() << "DB NOT OPEN";
        }

        QSqlTableModel *ret = new QSqlTableModel(this, _db);
        ret->setTable("modules");
        ret->setEditStrategy(QSqlTableModel::OnManualSubmit);

        return ret;
    }

    QSqlTableModel *Manager::operationsTable() {
        if (!_db.isOpen()) {
            qDebug() << "DB NOT OPEN";
        }

        QSqlTableModel *ret = new QSqlTableModel(this, _db);
        ret->setTable("operations");
        ret->setEditStrategy(QSqlTableModel::OnManualSubmit);

        return ret;
    }

    QSqlTableModel *Manager::resultsTable() {
        if (!_db.isOpen()) {
            qDebug() << "DB NOT OPEN";
        }

        QSqlTableModel *ret = new QSqlTableModel(this, _db);
        ret->setTable("results");
        ret->setEditStrategy(QSqlTableModel::OnManualSubmit);

        return ret;
    }

    QSqlTableModel *Manager::stringTable()
    {
        if (!_db.isOpen()) {
            qDebug() << "DB NOT OPEN";
        }

        QSqlTableModel *ret = new QSqlTableModel(this, _db);
        ret->setTable("strings");
        ret->setEditStrategy(QSqlTableModel::OnManualSubmit);

        return ret;

    }

    Manager::~Manager() {
        if (_db.isOpen()) {
            _db.close();
        }

        if (_serialPort) {
            delete _serialPort;
        }
    }

    QString Manager::dppDir()
    {
        if (!_dppDir.isNull()) {
            return _dppDir;
        }

        const char *dppDirEnv = getenv("DPP_DIR");

        if (dppDirEnv) {
            _dppDir = QString(dppDirEnv);
        } else {
            _dppDir = DPP_DIR;
        }

        _dppDir = expandTilde(_dppDir);

        QDir ourDir(_dppDir);
        if (!ourDir.exists()) {
            if (!ourDir.mkpath(".")) {
                qErr << "Couldn't create DPP DIR" << endl;
            }
        }

        return _dppDir;
    }

    QString Manager::jsonDir()
    {
        QStringList jsonSearchPath;
        jsonSearchPath.append(QString("%1%2%3").arg(QDir::currentPath()).arg(QDir::separator()).arg(DPP_JSON_PATH));
        jsonSearchPath.append(expandTilde(QString("~%1dpp%2%3").arg(QDir::separator()).arg(QDir::separator()).arg("json")));
        jsonSearchPath.append(QString("%1%2%3").arg(dppDir()).arg(QDir::separator()).arg(DPP_JSON_PATH));
        jsonSearchPath.append(expandTilde(QString("~%1ds2%2%3").arg(QDir::separator()).arg(QDir::separator()).arg("json")));

        const char *jsonEnv = getenv("DPP_JSON_DIR");
        if (jsonEnv) {
            jsonSearchPath.prepend(expandTilde(QString(jsonEnv)));
        }

        foreach (const QString &path, jsonSearchPath) {
            QDir ourDir(path);
            if (ourDir.exists()) {
                return path;
            }
        }

        qErr << "Could not find a DPP JSON directory in: " << jsonSearchPath.join(", ") << endl;
        throw "Unable to find DPP JSON directory";
    }

    DS2PacketPtr Manager::query(DS2PacketPtr aPacket)
    {
        DS2PacketPtr ret(new DS2Packet);

        _serialPort->setRequestToSend(true);
        // Send query to the ECU
        _serialPort->write((QByteArray)*aPacket);

        // Read the echo back.  We should check to see if it matches, maybe...
        waitForThisManyBytes(*_serialPort, aPacket->data().length() + 3);
        _serialPort->read(aPacket->data().length() + 3);

        // Read the result, save to thePacket
        quint8 ecuAddress;
        quint8 length;

        QByteArray inputArray;

        waitForThisManyBytes(*_serialPort, 2);
        inputArray = _serialPort->read(2);
        if (inputArray.length() != 2) {
            qDebug() << "Wanted length 2, got: " << inputArray.length();
            throw "ugh";
        }
        ecuAddress = inputArray.at(0);
        ret->setAddress(ecuAddress);
        length = inputArray.at(1);

        if (length <= 2) {
            throw new SerialIOError(QString("Ack. Got garbage data, length must be >= 2.  Got ECU: %1, LEN: %2").arg(QString::number(ecuAddress, 16)).arg(QString::number(length, 16)));
        }

        waitForThisManyBytes(*_serialPort, length - 2);
        inputArray = _serialPort->read(length - 2);
        if (inputArray.length() != (length - 2)) {
            qDebug() << "RUH ROH";
        }

        // Copy in all but the checksum.
        ret->setData(inputArray.mid(0, inputArray.length() - 1));

        // Verify the checksum
        unsigned char realChecksum = inputArray.at(inputArray.length() - 1);
#if 0
        qDebug() << QString("Checksum A: 0x%1").arg(ret->checksum(), 2, 16, QChar('0'));
        qDebug() << QString("Checksum B: 0x%1").arg(realChecksum, 2, 16, QChar('0'));
#endif

        if (ret->checksum() != realChecksum) {
            qDebug() << "Checksum mismatch...";
        }

        return ret;
    }

    ControlUnitPtr Manager::findModuleAtAddress(quint8 anAddress) {
        DS2PacketPtr ourSentPacket(new DS2Packet(anAddress, QByteArray((int)1, (unsigned char)0x00)));
        DS2PacketPtr ourReceivedPacket = query(ourSentPacket);
        qDebug() << "Our IDENT: " << *ourReceivedPacket;
        return findModuleByMatchingIdentPacket(ourReceivedPacket);
    }

    ControlUnitPtr Manager::findModuleByMatchingIdentPacket(const DS2PacketPtr aPacket) {
        ControlUnitPtr ret;
        QHash<QString, ControlUnitPtr > modules;
        QHash<QString, ControlUnitPtr >::Iterator moduleIt;

        // TODO: use findAllModulesByAddress instead
        modules = findAllModules();
        for (moduleIt = modules.begin(); moduleIt != modules.end(); ++moduleIt) {
            ControlUnitPtr ecu(moduleIt.value());

            if (ecu->matches().empty()) {
                continue;
            }

            DS2Response response(ecu->parseOperation("identify", aPacket));
            if (getenv("DPP_TRACE")) {
                qDebug() << "Checking " << ecu->name();
            }

            int success = 0;
            QHash<QString, QVariant>::ConstIterator matchIt;
            for (matchIt = ecu->matches().begin(); matchIt != ecu->matches().end(); ++matchIt) {
                QVariant a = matchIt.value();
                QVariant b = response.value(matchIt.key());
                if (getenv("DPP_TRACE")) {
                    qDebug() << "Comparing KEY: " << matchIt.key() << "A: " << a << " b: " << b;
                }
                success += (a == b) ? 1 : -1;
            }
            if (getenv("DPP_TRACE")) {
                qDebug() << "\tFound:" << moduleIt.key()  << ": " << ecu->name() << " success: " << success << " matches: " << ecu->matches().count();
            }
            if (success == ecu->matches().count()) {
                ret = ecu;
                modules.remove(moduleIt.key());
                break;
            }
        }

        modules.clear();

        return ret;
    }

    QHash<QString, QVariant> Manager::findModuleRecordByUuid(const QString &aUuid)
    {
        QHash<QString, QVariant> ret;
        QSqlRecord rec;
        QSqlTableModel *ourModel = modulesTable();

        ourModel->setFilter(QString("uuid = '%1'").arg(aUuid));
        ourModel->select();

        if (ourModel->rowCount() == 1) {
            rec = ourModel->record(0);
        }

        if (!rec.isEmpty()) {
            for (int i=0; i < ourModel->columnCount(); i++) {
                ret.insert(rec.fieldName(i), rec.value(i));
            }
        }

        delete ourModel;
        return ret;
    }

    QHash<QString, ControlUnitPtr > Manager::findAllModulesByFamily(const QString &aFamily)
    {
        QHash<QString, ControlUnitPtr > ret;
        QSqlTableModel *ourModel = modulesTable();

        ourModel->setFilter(QString("family= '%1'").arg(aFamily));
        ourModel->select();

        for (int i=0; i < ourModel->rowCount(); i++) {
            QSqlRecord theRecord = ourModel->record(i);
            QString uuid(theRecord.value(theRecord.indexOf("uuid")).toString());
            ControlUnitPtr ecu(new ControlUnit(uuid, this));
            ret.insert(uuid, ecu);
        }

        delete ourModel;
        return ret;
    }

    QHash<QString, ControlUnitPtr > Manager::findAllModules()
    {
        QHash<QString, ControlUnitPtr > ret;
        QSqlTableModel *ourModel = modulesTable();

        ourModel->setFilter(QString::null);
        ourModel->select();

        for (int i=0; i < ourModel->rowCount(); i++) {
            QSqlRecord ourRecord = ourModel->record(i);
            QString uuid(ourRecord.value(ourRecord.indexOf("uuid")).toString());
            ControlUnitPtr ecu(new ControlUnit(uuid, this));
            ret.insert(uuid, ecu);
        }

        delete ourModel;
        return ret;
    }


    bool Manager::removeModuleByUuid(const QString &aUuid)
    {
        bool ret = false;
        QSqlTableModel *ourModel = modulesTable();

        ourModel->setFilter(QString("uuid = '%1'").arg(aUuid));
        ourModel->select();

        if (ourModel->rowCount() == 1) {
            ourModel->removeRow(0);
            ret = ourModel->submitAll();
        }

        delete ourModel;
        return ret;
    }

    QHash<QString, QVariant> Manager::findOperationByUuid(const QString &aUuid)
    {
        QHash<QString, QVariant> ret;
        QSharedPointer<QSqlTableModel> ourModel(operationsTable());

        ourModel->setFilter(QString("uuid = '%1'").arg(aUuid));
        ourModel->select();

        if (ourModel->rowCount() == 1) {
            QSqlRecord theRecord = ourModel->record(0);
            for (int i=0; i < ourModel->columnCount(); i++) {
                ret.insert(theRecord.fieldName(i), theRecord.value(i));
            }
        }

        return ret;
    }

    bool Manager::removeOperationByUuid(const QString &aUuid)
    {
        QSharedPointer<QSqlTableModel> ourModel(operationsTable());

        ourModel->setFilter(QString("uuid = '%1'").arg(aUuid));
        ourModel->select();

        if (ourModel->rowCount() == 1) {
            ourModel->removeRows(0, 1);
            return ourModel->submitAll();
        } else {
            qDebug() << "Expected to find 1 row, found: " << ourModel->rowCount();
        }

        return false;
    }

    QHash<QString, QVariant> Manager::findResultByUuid(const QString &aUuid)
    {
        QHash<QString, QVariant> ret;
        QSharedPointer<QSqlTableModel> ourModel(resultsTable());

        ourModel->setFilter(QString("uuid = '%1'").arg(aUuid));
        ourModel->select();

        if (ourModel->rowCount() == 1) {
            QSqlRecord theRecord = ourModel->record(0);
            for (int i=0; i < ourModel->columnCount(); i++) {
                ret.insert(theRecord.fieldName(i), theRecord.value(i));
            }
        }

        return ret;
    }

    bool Manager::removeResultByUuid(const QString &aUuid)
    {
        QSharedPointer<QSqlTableModel> ourModel(resultsTable());

        ourModel->setFilter(QString("uuid = '%1'").arg(aUuid));
        ourModel->select();

        if (ourModel->rowCount() == 1) {
            ourModel->removeRow(0);
            return ourModel->submitAll();
        }

        return false;
    }

    bool Manager::parseOperationJson(const QJsonObject::ConstIterator &operationIt, QJsonObject moduleJSON, QSharedPointer<QSqlTableModel> &operationsModel)
    {
        QJsonObject ourOperation = operationIt.value().toObject();
        QSqlRecord operationRecord = operationsModel->record();

        QString uuid(ourOperation["uuid"].toString());

        QHash<QString, QVariant> oldOperation = findOperationByUuid(uuid);
        if (!oldOperation.isEmpty()) {
            if (getenv("DPP_TRACE")) {
                qDebug() << "Operation exists, overwriting " << uuid;
            }
            removeOperationByUuid(uuid);
        }

        operationRecord.setValue(operationRecord.indexOf("uuid"), uuid);
        operationRecord.setValue(operationRecord.indexOf("module_id"), moduleJSON["uuid"].toString());
        operationRecord.setValue(operationRecord.indexOf("name"), operationIt.key());

        QStringList commandByteList;
        foreach (const QJsonValue &value, ourOperation["command"].toArray()) {
            commandByteList.append(value.toString());
        }

        operationRecord.setValue(operationRecord.indexOf("command"), QVariant(commandByteList.join(",")));

        QSharedPointer<QSqlTableModel> ourModel(operationsTable());
        if (!ourModel->insertRecord(-1, operationRecord)) {
            qDebug() << "insertOpsRecord failed: " << ourModel->lastError() << endl;
            return false;
        }
        if (!ourModel->submitAll()) {
            qDebug() << "submitOpsRecords Failed: " << ourModel->lastError() << endl;
            return false;
        }

        quint64 resultsCount = 0;
        QJsonObject ourResults = ourOperation["results"].toObject();
        QJsonObject::ConstIterator resultIt = ourResults.begin();
        QSharedPointer<QSqlTableModel> ourResultsModel(resultsTable());
        while (resultIt != ourResults.end()) {
            if (!parseResultJson(resultIt, ourOperation, ourResultsModel)) {
                throw "Error parsing result JSON";
            }
            resultIt++;
            resultsCount++;
        }

        qErr << "\tAdded operation '" << operationIt.key() << "', returns " << resultsCount << " results" << endl;
        return true;
    }

    bool Manager::parseResultJson(const QJsonObject::ConstIterator &aResultIterator, QJsonObject operationJSON, QSharedPointer<QSqlTableModel> &aResultsModel)
    {
        QJsonObject ourResult = aResultIterator.value().toObject();
        QSqlRecord resultRecord = aResultsModel->record();

        QString uuid(ourResult["uuid"].toString());

        QHash<QString, QVariant> oldResult = findResultByUuid(uuid);
        if (!oldResult.isEmpty()) {
            if (getenv("DPP_TRACE")) {
                qDebug() << "Result exists, overwriting " << uuid;
            }
            removeResultByUuid(uuid);
        }

        resultRecord.setValue(resultRecord.indexOf("uuid"),         uuid);
        resultRecord.setValue(resultRecord.indexOf("operation_id"), operationJSON["uuid"].toString());
        resultRecord.setValue(resultRecord.indexOf("name"),         aResultIterator.key());
        resultRecord.setValue(resultRecord.indexOf("type"),         ourResult["type"].toString());
        resultRecord.setValue(resultRecord.indexOf("display"),      ourResult["display"].toString());
        resultRecord.setValue(resultRecord.indexOf("start_pos"),    ourResult["start_pos"].toInt());
        resultRecord.setValue(resultRecord.indexOf("length"),       ourResult["length"].toInt());
        resultRecord.setValue(resultRecord.indexOf("mask"),         ourResult["mask"].toString());
        resultRecord.setValue(resultRecord.indexOf("factor_a"),     ourResult["factor_a"].toDouble());
        resultRecord.setValue(resultRecord.indexOf("factor_b"),     ourResult["factor_b"].toDouble());
        resultRecord.setValue(resultRecord.indexOf("yes_value"),    ourResult["yes_value"].toString());
        resultRecord.setValue(resultRecord.indexOf("no_value"),     ourResult["no_value"].toString());

        if (!aResultsModel->insertRecord(-1, resultRecord)) {
            qDebug() << "insertResultRecord failed: " << aResultsModel->lastError() << endl;
            return false;
        }
        if (!aResultsModel->submitAll()) {
            qDebug() << "submitResultRecords Failed: " << aResultsModel->lastError() << endl;
            return false;
        }

        return true;
    }

    void Manager::initializeDatabase()
    {
        if (!_db.tables().contains("modules")) {
            qDebug() << "Need to create modules table";
            QSqlQuery query(_db);
            bool ret = query.exec(""                                            \
                                  "CREATE TABLE modules (\n"                    \
                                      "dpp_version  INTEGER NOT NULL,\n"        \
                                      "file_version INTEGER NOT NULL,\n"        \
                                      "uuid         VARCHAR UNIQUE NOT NULL PRIMARY KEY,\n" \
                                      "address      INTEGER,\n"                 \
                                      "family       VARCHAR,\n"                 \
                                      "name         VARCHAR NOT NULL,\n"        \
                                      "parent_id    VARCHAR,\n"                 \
                                      "matches      VARCHAR\n"                  \
                                  ");"                                          \
                    );

            if (!ret) {
                qDebug() << "Problem creating the modules table: " << query.lastError().driverText();
                throw "Problem creating the modules table";
            }
        }

        if (!_db.tables().contains("operations")) {
            qDebug() << "Need to create operations table";
            QSqlQuery query(_db);
            bool ret = query.exec("" \
                                  "CREATE TABLE operations (\n"              \
                                      "uuid      VARCHAR UNIQUE NOT NULL PRIMARY KEY,\n" \
                                      "module_id VARCHAR NOT NULL,\n"        \
                                      "name      VARCHAR NOT NULL,\n"        \
                                      "command   VARCHAR NOT NULL\n"         \
                                      ");"                                   \
                                 );
            if (!ret) {
                qDebug() << query.lastError().driverText();
                throw "Problem creating the modules table";
            }
        }

        if (!_db.tables().contains("results")) {
            qDebug() << "Need to create results table";
            QSqlQuery query(_db);
            bool ret = query.exec("" \
                                  "CREATE TABLE results (\n"                    \
                                      "uuid         VARCHAR UNIQUE NOT NULL PRIMARY KEY,\n" \
                                      "operation_id VARCHAR NOT NULL,\n"        \
                                      "name         VARCHAR NOT NULL,\n"        \
                                      "type         VARCHAR NOT NULL,\n"        \
                                      "display      VARCHAR NOT NULL,\n"        \
                                      "start_pos    INTEGER NOT NULL,\n"        \
                                      "length       INTEGER NOT NULL,\n"        \
                                      "mask         INTEGER,\n"                 \
                                      "factor_a     NUMERIC,\n"                 \
                                      "factor_b     NUMERIC,\n"                 \
                                      "yes_value    VARCHAR,\n"                 \
                                      "no_value     VARCHAR\n"                  \
                                      ");"                                      \
                                  );
            if (!ret) {
                qDebug() << query.lastError().driverText();
                throw "Problem creating the modules table";
            }
        }

        if (!_db.tables().contains("strings")) {
            qDebug() << "Need to create strings table";
            QSqlQuery query(_db);
            bool ret = query.exec("" \
                                  "CREATE TABLE strings (\n"                              \
                                      "table_uuid VARCHAR NOT NULL,\n"                    \
                                      "table_name VARCHAR NOT NULL,\n"                    \
                                      "number     INTEGER NOT NULL,\n"                    \
                                      "string     VARCHAR NOT NULL,\n"                    \
                                      "PRIMARY KEY (table_uuid, number)\n"                \
                                      ");"                                                \
                                 );
            if (!ret) {
                qDebug() << query.lastError().driverText();
                throw "Problem creating the strings table";
            }
        }

        QStringList jsonGlob("*.json");
        QDirIterator it(jsonDir(), jsonGlob, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            QJsonDocument jsonDoc;
            QFile jsonFile;
            QString path = it.next();
            QJsonParseError jsonError;

            qErr << "Parsing: " << it.fileName();
            jsonFile.setFileName(path);
            jsonFile.open(QIODevice::ReadOnly | QIODevice::Text);
            jsonDoc = QJsonDocument::fromJson(jsonFile.readAll(), &jsonError);
            jsonFile.close();

            if (jsonError.error != QJsonParseError::NoError) {
                qErr << endl << "\tError parsing " << it.fileName() << ": " <<jsonError.errorString() << endl << endl;
                continue;
            }

            QString fileType = jsonDoc.object()["file_type"].toString();
            QString uuid = jsonDoc.object()["uuid"].toString();
            qErr << " (" << uuid << ")" << endl;
            if (fileType == "string_table") {
                parseStringTableFile(jsonDoc.object());
            } else if (fileType == "ecu") {
                parseEcuFile(jsonDoc.object());
            }
        }
    }

    QString Manager::findStringByTableAndNumber(const QString &aStringTable, int aNumber)
    {
        QSharedPointer<QSqlTableModel> ourStringTable(stringTable());
        ourStringTable->setFilter(QString("((table_name = '%1') OR (table_uuid = '%2')) AND (number = %3)").arg(aStringTable).arg(aStringTable).arg(aNumber));
        ourStringTable->select();

        if (ourStringTable->rowCount() == 1) {
           QSqlRecord ourRecord = ourStringTable->record(0);
           return ourRecord.value(ourRecord.indexOf("string")).toString();
        }
        return QString::null;
    }

    void Manager::parseStringTableFile(const QJsonObject &aJsonObject)
    {
        QSharedPointer<QSqlTableModel> ourModel(stringTable());
        QJsonObject stringJson = aJsonObject;

        QString uuid(stringJson["uuid"].toString());
        QString tableName(stringJson["table_name"].toString());

        qErr << "\tFound table: " << tableName << endl;

        QSqlQuery removeThisStringTableQuery(_db);
        removeThisStringTableQuery.prepare("DELETE FROM strings WHERE table_uuid = :uuid");
        removeThisStringTableQuery.bindValue(":uuid", uuid);
        removeThisStringTableQuery.exec();

        QJsonObject ourStrings = stringJson["strings"].toObject();
        QJsonObject::Iterator stringIterator = ourStrings.begin();

        quint64 stringCount = 0;
        while (stringIterator != ourStrings.end()) {
            QJsonValue ourString = stringIterator.value();
            QSqlRecord stringRecord = ourModel->record();
            bool ok;
            quint8 stringNumber = stringIterator.key().toUInt(&ok, 16);

            stringRecord.setValue(stringRecord.indexOf("table_uuid"), uuid);
            stringRecord.setValue(stringRecord.indexOf("table_name"), tableName);
            stringRecord.setValue(stringRecord.indexOf("number"), stringNumber);
            stringRecord.setValue(stringRecord.indexOf("string"), ourString.toString());

            if (!ourModel->insertRecord(-1, stringRecord)) {
                qDebug() << "insertRecord failed: " << ourModel->lastError();
            }

            if (!ourModel->submitAll()) {
                qDebug() << "Saving the string(" << uuid << " / "<< ourString << ourModel->lastError();
                throw "Saving module failed";
            }

            stringIterator++;
            stringCount++;
        }
        qErr << "\tAdded " << stringCount << " strings" << endl << endl;
    }

    void Manager::parseEcuFile(const QJsonObject &aJsonObject)
    {
        QSharedPointer<QSqlTableModel> ourModel(modulesTable());
        QJsonObject moduleJson = aJsonObject;

        QString uuid(moduleJson["uuid"].toString());
        QString name(moduleJson["name"].toString());
        QString parent_id(moduleJson["parent_id"].toString());

        QHash<QString, QVariant> oldModule = findModuleRecordByUuid(uuid);
        if (!oldModule.isEmpty()) {
            if (getenv("DPP_TRACE")) {
                qDebug() << "\tModule exists, overwriting " << uuid;
            }
            removeModuleByUuid(uuid);
        }

        QSqlRecord moduleRecord = ourModel->record();
        moduleRecord.setValue(moduleRecord.indexOf("uuid"), uuid);
        moduleRecord.setValue(moduleRecord.indexOf("parent_id"), parent_id);
        moduleRecord.setValue(moduleRecord.indexOf("file_version"), moduleJson["file_version"].toInt());
        moduleRecord.setValue(moduleRecord.indexOf("dpp_version"), moduleJson["dpp_version"].toInt());
        moduleRecord.setValue(moduleRecord.indexOf("name"), name);
        moduleRecord.setValue(moduleRecord.indexOf("family"), moduleJson["family"].toString());

        QString ecuAddressString = moduleJson["address"].toString();
        if (!ecuAddressString.isEmpty()) {
            bool ok;
            quint8 ecuAddressNumber = ecuAddressString.toInt(&ok, 16);
            if (ok) {
                moduleRecord.setValue(moduleRecord.indexOf("address"), ecuAddressNumber);
            } else {
                qDebug() << "Error reading module JSON, invalid address " << ecuAddressString;
                throw "Error reading module JSON, invalid address";
            }
        } else {
            moduleRecord.setValue(moduleRecord.indexOf("address"), "");
        }

        qErr << "\tFound module: " << name;
        if (!ecuAddressString.isEmpty()) {
            qErr << " at " << ecuAddressString;
        }
        qErr << endl;
        if (!parent_id.isEmpty()) {
            qErr << "\tInherits: " << parent_id << endl;
        }

        QJsonObject ourMatches = moduleJson["matches"].toObject();
        QJsonObject::Iterator matchIterator = ourMatches.begin();
        QStringList matchStringList;
        QString matchString;
        while (matchIterator != ourMatches.end()) {
            QJsonValue match = matchIterator.value();
            if (match.isString()) {
                matchStringList.append(QString("%1=s:%2").arg(matchIterator.key()).arg(matchIterator.value().toString()));
            } else {
                matchStringList.append(QString("%1=i:%2").arg(matchIterator.key()).arg(matchIterator.value().toInt()));
            }
            matchIterator++;
        }
        matchString = matchStringList.join(";");
        moduleRecord.setValue(moduleRecord.indexOf("matches"), matchString);

        quint64 operationsCount = 0;
        QJsonObject ourOperations = moduleJson["operations"].toObject();
        QJsonObject::Iterator operationIterator = ourOperations.begin();
        QSharedPointer<QSqlTableModel> ourOperationsTable(operationsTable());
        while (operationIterator != ourOperations.end()) {
            if (!parseOperationJson(operationIterator, moduleJson, ourOperationsTable)) {
                throw "Error parsing the operation";
            }
            operationIterator++;
            operationsCount++;
        }
        qErr << "\tTotal: " << operationsCount << " operations" << endl;

        if (!ourModel->insertRecord(-1, moduleRecord)) {
            qDebug() << "insertRecord failed: " << ourModel->lastError();
        }

        if (!ourModel->submitAll()) {
            qDebug() << "Saving the module(" << uuid << " " << ourModel->lastError();
            throw "Saving module failed";
        }
        qErr << endl;
    }
}
