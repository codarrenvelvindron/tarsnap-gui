#include "taskmanager.h"
#include "utils.h"
#include "debug.h"

#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QTimer>

#define SUCCESS 0

TaskManager::TaskManager() : _threadPool(QThreadPool::globalInstance()),
    _aggressiveNetworking(false), _preservePathnames(true), _headless(false)
{
    // Move the operations belonging to the Task manager to a separate thread
    _managerThread.start();
    moveToThread(&_managerThread);
}

TaskManager::~TaskManager()
{
    _managerThread.quit();
    _managerThread.wait();
}

bool TaskManager::headless() const
{
    return _headless;
}

void TaskManager::setHeadless(bool headless)
{
    _headless = headless;
}

void TaskManager::loadSettings()
{
    QSettings settings;

    _tarsnapDir             = settings.value("tarsnap/path").toString();
    _tarsnapVersion         = settings.value("tarsnap/version").toString();
    _tarsnapCacheDir        = settings.value("tarsnap/cache").toString();
    _tarsnapKeyFile         = settings.value("tarsnap/key").toString();
    _aggressiveNetworking   = settings.value("tarsnap/aggressive_networking", false).toBool();
    _preservePathnames      = settings.value("tarsnap/preserve_pathnames", true).toBool();

    // First time init of the Store
    PersistentStore::instance();
}

void TaskManager::getTarsnapVersion(QString tarsnapPath)
{
    TarsnapClient *tarsnap = new TarsnapClient();
    tarsnap->setCommand(tarsnapPath + QDir::separator() + CMD_TARSNAP);
    tarsnap->setArguments(QStringList("--version"));
    connect(tarsnap, &TarsnapClient::finished, this,
            &TaskManager::getTarsnapVersionFinished , QUEUED);
    queueTask(tarsnap);
}

void TaskManager::registerMachine(QString user, QString password, QString machine, QString key, QString tarsnapPath, QString cachePath)
{
    TarsnapClient *registerClient = new TarsnapClient();
    QStringList args;
    QFileInfo keyFile(key);
    if(keyFile.exists())
    {
        // existing key, just check with a tarsnap --print-stats command
        args << "--fsck" << "--keyfile" << key << "--cachedir" << cachePath;
        registerClient->setCommand(tarsnapPath + QDir::separator() + CMD_TARSNAP);
        registerClient->setArguments(args);
    }
    else
    {
        // register machine with tarsnap-keygen
        args << "--user" << user << "--machine" << machine << "--keyfile" << key;
        registerClient->setCommand(tarsnapPath + QDir::separator() + CMD_TARSNAPKEYGEN);
        registerClient->setArguments(args);
        registerClient->setPassword(password);
        registerClient->setRequiresPassword(true);
    }
    connect(registerClient, &TarsnapClient::finished, this,
            &TaskManager::registerMachineFinished, QUEUED);
    queueTask(registerClient);
}

void TaskManager::backupNow(BackupTaskPtr backupTask)
{
    if(backupTask == NULL)
    {
        DEBUG << "Null BackupTaskPtr passed.";
        return;
    }

    _backupTaskMap[backupTask->uuid()] = backupTask;
    TarsnapClient *backupClient = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    if(_aggressiveNetworking)
        args << "--aggressive-networking";
    if(backupTask->optionDryRun())
        args << "--dry-run";
    if(backupTask->optionSkipNoDump())
        args << "--nodump";
    if(backupTask->optionPreservePaths())
        args << "-P";
    if(!backupTask->optionTraverseMount())
        args << "--one-file-system";
    if(backupTask->optionFollowSymLinks())
        args << "-L";
    QRegExp versionRx("(\\d+\\.\\d+\\.\\d+(\\.\\d+)?)");
    if((-1 != versionRx.indexIn(_tarsnapVersion)) && (versionRx.cap(0) >= "1.0.36"))
        args << "--creationtime" << QString::number(backupTask->timestamp().toTime_t());
    args << "--quiet" << "--print-stats" << "--no-humanize-numbers" << "-c"
         << "-f" << backupTask->name();
    foreach(QString exclude, backupTask->getExcludesList())
    {
        args << "--exclude" << exclude;
    }
    foreach(QUrl url, backupTask->urls())
    {
        args << url.toLocalFile();
    }
    backupClient->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    backupClient->setArguments(args);
    backupClient->setData(backupTask->uuid());
    connect(backupClient, &TarsnapClient::finished, this,
            &TaskManager::backupTaskFinished, QUEUED);
    connect(backupClient, &TarsnapClient::started, this,
            &TaskManager::backupTaskStarted, QUEUED);
    backupTask->setStatus(TaskStatus::Queued);
    queueTask(backupClient, true);
}

void TaskManager::loadArchives()
{
    // Load from PersistentStore first
    _archiveMap.clear();
    PersistentStore& store = PersistentStore::instance();
    if(!store.initialized())
    {
        DEBUG << "PersistentStore was not initialized properly.";
        return;
    }
    QSqlQuery query = store.createQuery();
    if(!query.prepare(QLatin1String("select name from archives")))
    {
        DEBUG << query.lastError().text();
        return;
    }
    if(store.runQuery(query) && query.next())
    {
        do
        {
            ArchivePtr archive(new Archive);
            archive->setName(query.value(query.record().indexOf("name")).toString());
            archive->load();
            _archiveMap[archive->name()] = archive;
        } while(query.next());
    }
    emit archiveList(_archiveMap.values());

    // Issue sync from remote next
    TarsnapClient *listArchivesClient = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty()) // We shouldn't need to pass this as per the man page, however Tarsnap CLI seems to require it
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--list-archives" << "-vv";
    listArchivesClient->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    listArchivesClient->setArguments(args);
    connect(listArchivesClient, &TarsnapClient::finished, this,
            &TaskManager::getArchiveListFinished, QUEUED);
    queueTask(listArchivesClient);
}

void TaskManager::getArchiveStats(ArchivePtr archive)
{
    if(archive.isNull())
    {
        DEBUG << "Null ArchivePtr passed.";
        return;
    }

    _archiveMap.insert(archive->name(), archive);

    TarsnapClient *statsClient = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--print-stats" << "--no-humanize-numbers" << "-f" << archive->name();
    statsClient->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    statsClient->setArguments(args);
    statsClient->setData(archive->name());
    connect(statsClient, &TarsnapClient::finished, this,
            &TaskManager::getArchiveStatsFinished, QUEUED);
    queueTask(statsClient);
}

void TaskManager::getArchiveContents(ArchivePtr archive)
{
    if(archive.isNull())
    {
        DEBUG << "Null ArchivePtr passed.";
        return;
    }

    _archiveMap.insert(archive->name(), archive);

    TarsnapClient *contentsClient = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty()) // We shouldn't need to pass this as per the man page, however Tarsnap CLI seems to require it
        args << "--cachedir" << _tarsnapCacheDir;
    if(_preservePathnames)
        args << "-P";
    args << "-t" << "-f" << archive->name();
    contentsClient->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    contentsClient->setArguments(args);
    contentsClient->setData(archive->name());
    connect(contentsClient, &TarsnapClient::finished, this,
            &TaskManager::getArchiveContentsFinished, QUEUED);
    queueTask(contentsClient);
}

void TaskManager::deleteArchives(QList<ArchivePtr> archives)
{
    if(archives.isEmpty())
    {
        DEBUG << "Empty QList<ArchivePtr> passed.";
        return;
    }

    TarsnapClient *delArchives = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--print-stats" << "-d";
    foreach(ArchivePtr archive, archives)
    {
        args << "-f" << archive->name();
    }
    delArchives->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    delArchives->setArguments(args);
    delArchives->setData(QVariant::fromValue(archives));
    connect(delArchives, &TarsnapClient::finished, this,
            &TaskManager::deleteArchivesFinished, QUEUED);
    queueTask(delArchives, true);
}

void TaskManager::getOverallStats()
{
    TarsnapClient *overallStats = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--print-stats" << "--no-humanize-numbers";
    overallStats->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    overallStats->setArguments(args);
    connect(overallStats, &TarsnapClient::finished, this,
            &TaskManager::overallStatsFinished, QUEUED);
    queueTask(overallStats);
}

void TaskManager::fsck()
{
    TarsnapClient *fsck = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--fsck-prune";
    fsck->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    fsck->setArguments(args);
    connect(fsck, &TarsnapClient::finished, this,
            &TaskManager::fsckFinished, QUEUED);
    queueTask(fsck, true);
}

void TaskManager::nuke()
{
    TarsnapClient *nuke = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--nuke";
    nuke->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    nuke->setPassword("No Tomorrow");
    nuke->setRequiresPassword(true);
    nuke->setArguments(args);
    connect(nuke, &TarsnapClient::finished, this,
            &TaskManager::nukeFinished, QUEUED);
    queueTask(nuke, true);
}

void TaskManager::restoreArchive(ArchivePtr archive, ArchiveRestoreOptions options)
{
    if(archive.isNull())
    {
        DEBUG << "Null ArchivePtr passed.";
        return;
    }

    _archiveMap.insert(archive->name(), archive);

    TarsnapClient *restore = new TarsnapClient();
    QStringList args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(options.preservePaths)
        args << "-P";
    if(!options.chdir.isEmpty())
        args << "-C" << options.chdir;
    if(!options.overwriteFiles)
        args << "-k";
    if(options.keepNewerFiles)
        args << "--keep-newer-files";
    args << "-x" << "-f" << archive->name();
    restore->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    restore->setArguments(args);
    restore->setData(archive->name());
    connect(restore, &TarsnapClient::finished, this,
            &TaskManager::restoreArchiveFinished, QUEUED);
    queueTask(restore);
}

void TaskManager::runScheduledJobs()
{
    loadJobs();
    bool nothingToDo = true;
    foreach(JobPtr job, _jobMap)
    {
        if(job->optionScheduledEnabled())
        {
            backupNow(job->createBackupTask());
            nothingToDo = false;
        }
    }
    if(nothingToDo)
        qApp->quit();
}

void TaskManager::stopTasks(bool running, bool queued)
{
    if(queued) // queued should be cleared first
    {
        _taskQueue.clear();
    }
    if(running)
    {
        foreach(TarsnapClient *client, _runningTasks)
        {
            if(client) client->stop();
        }
    }
}

void TaskManager::backupTaskFinished(QVariant data, int exitCode, QString output)
{
    BackupTaskPtr backupTask = _backupTaskMap[data.toUuid()];
    backupTask->setExitCode(exitCode);
    backupTask->setOutput(output);
    if(exitCode == SUCCESS)
    {
        auto client = qobject_cast<TarsnapClient*>(sender());
        ArchivePtr archive(new Archive);
        archive->setName(backupTask->name());
        archive->setCommand(client->command() + " " + client->arguments().join(" "));
        archive->setTimestamp(backupTask->timestamp());
        archive->setJobRef(backupTask->jobRef());
        parseArchiveStats(output, true, archive);
        backupTask->setArchive(archive);
        backupTask->setStatus(TaskStatus::Completed);
        _archiveMap.insert(archive->name(), archive);
        foreach(JobPtr job, _jobMap)
        {
            if(job->objectKey() == archive->jobRef())
                emit job->loadArchives();
        }
        emit archiveList(_archiveMap.values());
        parseGlobalStats(output);
    }
    else
    {
        backupTask->setStatus(TaskStatus::Failed);
    }
    _backupTaskMap.take(backupTask->uuid());
    notifyBackupTaskUpdate(backupTask);
}

void TaskManager::backupTaskStarted(QVariant data)
{
    BackupTaskPtr backupTask = _backupTaskMap[data.toString()];
    backupTask->setStatus(TaskStatus::Running);
    notifyBackupTaskUpdate(backupTask);
}

void TaskManager::registerMachineFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)
    if(exitCode == SUCCESS)
        emit registerMachineStatus(TaskStatus::Completed, output);
    else
        emit registerMachineStatus(TaskStatus::Failed, output);
}

void TaskManager::getArchiveListFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to list archives from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2").arg(exitCode).arg(output));
        return;
    }

    QMap<QString, ArchivePtr>   _newArchiveMap;
    QStringList lines = output.trimmed().split('\n');
    foreach(QString line, lines)
    {
        QRegExp archiveDetailsRX("^(.+)\\t+(\\S+\\s+\\S+)\\t+(.+)$");
        if(-1 != archiveDetailsRX.indexIn(line))
        {
            QStringList archiveDetails = archiveDetailsRX.capturedTexts();
            archiveDetails.removeFirst();
            QDateTime timestamp = QDateTime::fromString(archiveDetails[1], Qt::ISODate);
            ArchivePtr archive(new Archive);
            bool update = false;
            archive->setName(archiveDetails[0]);
            archive->load();
            if(archive->objectKey().isEmpty())
            {
                update = true;
            }
            else if(archive->timestamp() != timestamp)
            {
                // There is a different archive with the same name on the remote
                archive->purge();
                archive.clear();
                archive = archive.create();
                archive->setName(archiveDetails[0]);
                update = true;
            }
            if(update)
            {
                // New archive
                archive->setTimestamp(timestamp);
                archive->setCommand(archiveDetails[2]);
                archive->save();
                // Automagically set Job ownership
                foreach(JobPtr job, _jobMap)
                {
                    if(archive->name().startsWith(job->archivePrefix()))
                    {
                        archive->setJobRef(job->objectKey());
                    }
                }
                getArchiveStats(archive);
            }
            _newArchiveMap.insert(archive->name(), archive);
            _archiveMap.remove(archive->name());
        }
    }
    // Purge archives left in old _archiveMap (not mirrored by the remote)
    foreach(ArchivePtr archive, _archiveMap)
    {
        archive->purge();
    }
    _archiveMap.clear();
    _archiveMap = _newArchiveMap;
    foreach(JobPtr job, _jobMap)
    {
        emit job->loadArchives();
    }
    emit archiveList(_archiveMap.values(), true);
    getOverallStats();
}

void TaskManager::getArchiveStatsFinished(QVariant data, int exitCode, QString output)
{
    ArchivePtr archive = _archiveMap[data.toString()];

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to get archive stats from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2").arg(exitCode).arg(output));
        return;
    }

    if(archive)
    {
        parseArchiveStats(output, false, archive);
        parseGlobalStats(output);
    }
}

void TaskManager::getArchiveContentsFinished(QVariant data, int exitCode, QString output)
{
    ArchivePtr archive = _archiveMap[data.toString()];

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to get archive contents from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2").arg(exitCode).arg(output));
        return;
    }

    if(archive)
    {
        archive->setContents(output.trimmed().split('\n', QString::SkipEmptyParts));
        archive->save();
    }
}

void TaskManager::deleteArchivesFinished(QVariant data, int exitCode, QString output)
{
    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to delete archive from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2").arg(exitCode).arg(output));
        return;
    }

    QList<ArchivePtr> archives = data.value<QList<ArchivePtr>>();
    if(!archives.empty())
    {
        foreach(ArchivePtr archive, archives)
        {
            _archiveMap.remove(archive->name());
            archive->purge();
        }
        emit archiveList(_archiveMap.values());
        emit archivesDeleted(archives);
    }
    parseGlobalStats(output);
}

void TaskManager::overallStatsFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data);

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to get stats from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2").arg(exitCode).arg(output));
        return;
    }

    parseGlobalStats(output);
}

void TaskManager::fsckFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)
    if(exitCode == SUCCESS)
        emit fsckStatus(TaskStatus::Completed, output);
    else
        emit fsckStatus(TaskStatus::Failed, output);
}

void TaskManager::nukeFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)
    if(exitCode == SUCCESS)
        emit nukeStatus(TaskStatus::Completed, output);
    else
        emit nukeStatus(TaskStatus::Failed, output);
    fsck();
    loadArchives();
}

void TaskManager::restoreArchiveFinished(QVariant data, int exitCode, QString output)
{
    ArchivePtr archive = _archiveMap[data.toString()];
    if(archive && (exitCode == SUCCESS))
        emit restoreArchiveStatus(archive, TaskStatus::Completed, output);
    else
        emit restoreArchiveStatus(archive, TaskStatus::Failed, output);
}

void TaskManager::notifyBackupTaskUpdate(BackupTaskPtr backupTask)
{
    if(!_headless)
        return;
    switch (backupTask->status())
    {
    case TaskStatus::Completed:
        emit displayNotification(tr("Backup %1 completed. (%2 new data on Tarsnap)")
                            .arg(backupTask->name())
                            .arg(Utils::humanBytes(backupTask->archive()->sizeUniqueCompressed())));
        delete backupTask;
        break;
    case TaskStatus::Running:
        emit displayNotification(tr("Backup %1 is running.").arg(backupTask->name()));
        break;
    case TaskStatus::Failed:
        emit displayNotification(tr("Backup %1 failed: %2").arg(backupTask->name()).arg(backupTask->output().simplified()));
        delete backupTask;
        break;
    default:
        break;
    }
}

void TaskManager::queueTask(TarsnapClient *cli, bool exclusive)
{
    if(cli == NULL)
    {
        DEBUG << "NULL argument";
        return;
    }
    if(exclusive && !_runningTasks.isEmpty())
        _taskQueue.enqueue(cli);
    else
        startTask(cli);
}

void TaskManager::startTask(TarsnapClient *cli)
{
    if(cli == NULL)
    {
        if(!_taskQueue.isEmpty())
            cli = _taskQueue.dequeue();
        else
            return;
    }
    connect(cli, &TarsnapClient::terminated, this, &TaskManager::dequeueTask, QUEUED);
    _runningTasks.append(cli);
    cli->setAutoDelete(false);
    _threadPool->start(cli);
    emit idle(false);
}

void TaskManager::dequeueTask()
{
    _runningTasks.removeOne(qobject_cast<TarsnapClient*>(sender()));
    if(_runningTasks.isEmpty())
    {
        if(_taskQueue.isEmpty())
        {
            if(_headless)
                QTimer::singleShot(500, qApp, &QCoreApplication::quit); // Give a chance for notifications to go through
            else
                emit idle(true);
        }
        else
        {
            startTask(NULL); // start another queued task
        }
    }
}

void TaskManager::parseGlobalStats(QString tarsnapOutput)
{
    quint64 sizeTotal;
    quint64 sizeCompressed;
    quint64 sizeUniqueTotal;
    quint64 sizeUniqueCompressed;

    QStringList lines = tarsnapOutput.trimmed().split('\n', QString::SkipEmptyParts);
    if(lines.count() < 3)
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    QRegExp sizeRX("^All archives\\s+(\\d+)\\s+(\\d+)$");
    QRegExp uniqueSizeRX("^\\s+\\(unique data\\)\\s+(\\d+)\\s+(\\d+)$");
    if(-1 != sizeRX.indexIn(lines[1]))
    {
        QStringList captured = sizeRX.capturedTexts();
        captured.removeFirst();
        sizeTotal = captured[0].toULongLong();
        sizeCompressed = captured[1].toULongLong();
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    if(-1 != uniqueSizeRX.indexIn(lines[2]))
    {
        QStringList captured = uniqueSizeRX.capturedTexts();
        captured.removeFirst();
        sizeUniqueTotal = captured[0].toULongLong();
        sizeUniqueCompressed = captured[1].toULongLong();
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    emit overallStats(sizeTotal, sizeCompressed, sizeUniqueTotal, sizeUniqueCompressed
                      , _archiveMap.count());
}

void TaskManager::parseArchiveStats(QString tarsnapOutput, bool newArchiveOutput, ArchivePtr archive)
{
    QStringList lines = tarsnapOutput.trimmed().split('\n', QString::SkipEmptyParts);
    if(lines.count() != 5)
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    QString sizeLine = lines[3];
    QString uniqueSizeLine = lines[4];
    QRegExp sizeRX;
    QRegExp uniqueSizeRX;
    if(newArchiveOutput)
    {
        sizeRX.setPattern("^This archive\\s+(\\d+)\\s+(\\d+)$");
        uniqueSizeRX.setPattern("^New data\\s+(\\d+)\\s+(\\d+)$");
    }
    else
    {
        sizeRX.setPattern(QString("^%1\\s+(\\d+)\\s+(\\d+)$").arg(archive->name()));
        uniqueSizeRX.setPattern("^\\s+\\(unique data\\)\\s+(\\d+)\\s+(\\d+)$");
    }
    if(-1 != sizeRX.indexIn(sizeLine))
    {
        QStringList captured = sizeRX.capturedTexts();
        captured.removeFirst();
        archive->setSizeTotal(captured[0].toULongLong());
        archive->setSizeCompressed(captured[1].toULongLong());
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    if(-1 != uniqueSizeRX.indexIn(uniqueSizeLine))
    {
        QStringList captured = uniqueSizeRX.capturedTexts();
        captured.removeFirst();
        archive->setSizeUniqueTotal(captured[0].toULongLong());
        archive->setSizeUniqueCompressed(captured[1].toULongLong());
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    archive->save();
}

QString TaskManager::makeTarsnapCommand(QString cmd)
{
    if(_tarsnapDir.isEmpty())
        return cmd;
    else
        return _tarsnapDir + QDir::separator() + cmd;
}

void TaskManager::loadJobs()
{
    _jobMap.clear();
    PersistentStore& store = PersistentStore::instance();
    if(!store.initialized())
    {
        DEBUG << "PersistentStore was not initialized properly.";
        return;
    }
    QSqlQuery query = store.createQuery();
    if(!query.prepare(QLatin1String("select name from jobs")))
    {
        DEBUG << query.lastError().text();
        return;
    }
    if(store.runQuery(query) && query.next())
    {
        do
        {
            JobPtr job(new Job);
            job->setName(query.value(query.record().indexOf("name")).toString());
            connect(job.data(), &Job::loadArchives, this, &TaskManager::loadJobArchives, QUEUED);
            job->load();
            _jobMap[job->name()] = job;
        } while(query.next());
    }
    emit jobsList(_jobMap);
}

void TaskManager::deleteJob(JobPtr job, bool purgeArchives)
{
    if(job)
    {
        if(purgeArchives)
            deleteArchives(job->archives());
        job->purge();
        _jobMap.remove(job->name());
    }
}

void TaskManager::loadJobArchives()
{
    Job *job = qobject_cast<Job*>(sender());
    QList<ArchivePtr> archives;
    foreach(ArchivePtr archive, _archiveMap)
    {
        if(archive->jobRef() == job->objectKey())
        {
            archives << archive;
        }
    }
    job->setArchives(archives);
}

void TaskManager::getTaskInfo()
{
    emit taskInfo(_runningTasks.count(), _taskQueue.count());
}

void TaskManager::addJob(JobPtr job)
{
    _jobMap[job->name()] = job;
    connect(job.data(), &Job::loadArchives, this, &TaskManager::loadJobArchives, QUEUED);
}

void TaskManager::getTarsnapVersionFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to get Tarsnap version."),
                     tr("Tarsnap exited with code %1 and output:\n%2").arg(exitCode).arg(output));
        return;
    }

    QRegExp versionRx("^tarsnap (\\S+)\\s$");
    if(-1 != versionRx.indexIn(output))
        emit tarsnapVersion(versionRx.cap(1));
}
