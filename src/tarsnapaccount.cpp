#include "tarsnapaccount.h"
#include "debug.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTableWidget>

#define URL_ACTIVITY "https://www.tarsnap.com/manage.cgi?address=%1&password=%2&action=activity&format=csv"
#define URL_MACHINE_ACTIVITY "https://www.tarsnap.com/manage.cgi?address=%1&password=%2&action=subactivity&mid=%3&format=csv"
#define URL_LIST_MACHINES "https://www.tarsnap.com/manage.cgi?address=%1&password=%2&action=subactivity"

TarsnapAccount::TarsnapAccount(QWidget *parent) : QDialog(parent), _user(),
    _nam(this)
{
    _ui.setupUi(this);
}

QString TarsnapAccount::user() const
{
    return _user;
}

void TarsnapAccount::setUser(const QString &user)
{
    _user = user;
    _ui.textLabel->setText(QString("Type password for account %1:").arg(_user));
}

void TarsnapAccount::getAccountInfo(bool displayActivity, bool displayMachineActivity)
{
    if(exec())
    {
        QString getActivity(URL_ACTIVITY);
        getActivity = getActivity.arg(QString(QUrl::toPercentEncoding(_user)),
                                      QString(QUrl::toPercentEncoding(_ui.passwordLineEdit->text())));
        if(displayActivity)
            connect(tarsnapRequest(getActivity), SIGNAL(finished()), this, SLOT(readActivityCSVAndDisplay()));
        else
            connect(tarsnapRequest(getActivity), SIGNAL(finished()), this, SLOT(readActivityCSV()));
        QString getMachineId(URL_LIST_MACHINES);
        getMachineId = getMachineId.arg(QString(QUrl::toPercentEncoding(_user)),
                                        QString(QUrl::toPercentEncoding(_ui.passwordLineEdit->text())));
        if(displayMachineActivity)
            connect(tarsnapRequest(getMachineId), SIGNAL(finished()), this, SLOT(getMachineActivity()));
        else
            connect(tarsnapRequest(getMachineId), SIGNAL(finished()), this, SLOT(getMachineActivity()));
    }
}

void TarsnapAccount::getMachineActivity()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString html = reply->readAll();
    reply->close();
    reply->deleteLater();

    if(html.isEmpty())
        return;

    QString machineId;
    QRegExp machineIdRx("mid=(\\d+)\">" + QRegExp::escape(_machine) + "</a>", Qt::CaseSensitive, QRegExp::RegExp2);

    if(machineIdRx.lastIndexIn(html))
    {
        machineId = machineIdRx.cap(1);
    }
    else
    {
        DEBUG << "Machine id not found for machine " << _machine;
        return;
    }

    QString machineActivity(URL_MACHINE_ACTIVITY);
    machineActivity = machineActivity.arg(QString(QUrl::toPercentEncoding(_user)),
                                          QString(QUrl::toPercentEncoding(_ui.passwordLineEdit->text())),
                                          QString(QUrl::toPercentEncoding(machineId)));
    connect(tarsnapRequest(machineActivity), SIGNAL(finished()), this, SLOT(readMachineActivityCSVAndDisplay()));
}

void TarsnapAccount::displayCSVTable(QString csv)
{
    DEBUG << csv;
    if(csv.isEmpty())
        return;

    QStringList lines = csv.split(QRegExp("[\r\n]"), QString::SkipEmptyParts);
    if(lines.count() <= 1)
        return;

    QStringList columnHeaders = lines.first().split(',', QString::SkipEmptyParts);
    lines.removeFirst();

    QTableWidget *table = new QTableWidget(lines.count(), columnHeaders.count());
    table->setHorizontalHeaderLabels(columnHeaders);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QDialog *tableDialog = new QDialog(this);
    tableDialog->setAttribute( Qt::WA_DeleteOnClose, true );
    QHBoxLayout *layout = new QHBoxLayout;
    layout->addWidget(table);
    layout->setMargin(0);
    tableDialog->setLayout(layout);

    qint64 row = 0;
    qint64 column = 0;

    foreach (QString line, lines)
    {
        foreach (QString entry, line.split(',', QString::KeepEmptyParts))
        {
            table->setItem(row, column, new QTableWidgetItem(entry));
            column++;
        }
        row++;
        column = 0;
    }
    tableDialog->show();
}

QNetworkReply* TarsnapAccount::tarsnapRequest(QString url)
{
    QNetworkRequest request;
    request.setUrl(url);
    request.setRawHeader("User-Agent",
                         (qApp->applicationName() + " " + qApp->applicationVersion()).toLatin1());
    QNetworkReply *reply = _nam.get(request);
    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
            this, SLOT(networkError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
            this, SLOT(sslError(QList<QSslError>)));
    return reply;
}

void TarsnapAccount::parseCredit(QString csv)
{
    if(csv.isEmpty())
        return;
    QRegExp lastBalanceRx("^(Balance.+)$", Qt::CaseInsensitive, QRegExp::RegExp2);
    QString lastBalance;
    foreach (QString line, csv.split(QRegExp("[\r\n]"), QString::SkipEmptyParts))
    {
        if(0 == lastBalanceRx.indexIn(line))
            lastBalance = line;
    }

    if(!lastBalance.isEmpty())
    {
        QStringList fields = lastBalance.split(',', QString::SkipEmptyParts);
        if(fields.count() != 3)
        {
            DEBUG << "Invalid CSV.";
            return;
        }
        emit accountCredit(fields.last().toFloat(), QDate::fromString(fields[1], Qt::ISODate));
    }
}

void TarsnapAccount::parseLastMachineActivity(QString csv)
{
    if(csv.isEmpty())
        return;
    QString lastLine = csv.split(QRegExp("[\r\n]"), QString::SkipEmptyParts).last();
    emit lastMachineActivity(lastLine.split(',', QString::SkipEmptyParts));
}

void TarsnapAccount::readActivityCSV()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    parseCredit(reply->readAll());
    reply->close();
    reply->deleteLater();
}

void TarsnapAccount::readMachineActivityCSV()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    parseLastMachineActivity(reply->readAll());
    reply->close();
    reply->deleteLater();
}

void TarsnapAccount::readActivityCSVAndDisplay()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray replyData = reply->readAll();
    parseCredit(replyData);
    displayCSVTable(replyData);
    reply->close();
    reply->deleteLater();
}

void TarsnapAccount::readMachineActivityCSVAndDisplay()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray replyData = reply->readAll();
    parseLastMachineActivity(replyData);
    displayCSVTable(replyData);
    reply->close();
    reply->deleteLater();
}

void TarsnapAccount::networkError(QNetworkReply::NetworkError error)
{
    Q_UNUSED(error)
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    DEBUG << reply->errorString();
}

void TarsnapAccount::sslError(QList<QSslError> errors)
{
    foreach (QSslError error, errors)
    {
        DEBUG << error.errorString();
    }
}

QString TarsnapAccount::getMachine() const
{
    return _machine;
}

void TarsnapAccount::setMachine(const QString &machine)
{
    _machine = machine;
}

