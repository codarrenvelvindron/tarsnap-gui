#include "jobwidget.h"
#include "ui_jobwidget.h"
#include "debug.h"

JobWidget::JobWidget(QWidget *parent) :
    QWidget(parent),
    _ui(new Ui::JobWidget)
{
    _ui->setupUi(this);
    connect(_ui->listArchivesButton, &QPushButton::clicked,
            [=](){
                _ui->stackedWidget->setCurrentWidget(_ui->jobRestorePage);
            });
    connect(_ui->restoreBackButton, &QPushButton::clicked,
            [=](){
                _ui->stackedWidget->setCurrentWidget(_ui->jobDetailPage);
            });
    connect(_ui->optionsBackButton, &QPushButton::clicked,
            [=](){
                _ui->stackedWidget->setCurrentWidget(_ui->jobDetailPage);
            });
    connect(_ui->optionsButton, &QPushButton::clicked,
            [=](){
                _ui->stackedWidget->setCurrentWidget(_ui->jobOptionsPage);
            });
    connect(_ui->cancelButton, SIGNAL(clicked()), this, SIGNAL(cancel()));
    connect(_ui->restoreListWidget, SIGNAL(inspectArchive(ArchivePtr)), this, SIGNAL(inspectJobArchive(ArchivePtr)));
}

JobWidget::~JobWidget()
{
    delete _ui;
}
JobPtr JobWidget::job() const
{
    return _job;
}

void JobWidget::setJob(const JobPtr &job)
{
    if(_job && !_job->objectKey().isEmpty())
    {
        disconnect(_ui->detailTreeWidget, SIGNAL(selectionChanged()), this, SLOT(save()));
        disconnect(_job.data(), SIGNAL(changed()), this, SLOT(jobUpdate()));
    }
    _job = job;
    connect(_job.data(), SIGNAL(changed()), this, SLOT(jobUpdate()));
    updateDetails();
}

void JobWidget::save()
{
    DEBUG << "SAVE";
    // save current or new job details
    if(_job->objectKey().isEmpty())
    {
        _job->setName(_ui->jobNameLineEdit->text());
        _job->setUrls(_ui->newJobTreeWidget->getSelectedUrls());
        emit jobAdded(_job);
    }
    else
    {
        _job->setUrls(_ui->detailTreeWidget->getSelectedUrls());
    }
    _job->save();
}

void JobWidget::jobUpdate()
{
    if(_job)
    {
        setJob(_job);
    }
}

void JobWidget::updateDetails()
{
    if(_job->objectKey().isEmpty())
    {
        _ui->stackedWidget->setCurrentWidget(_ui->jobNewPage);
        _ui->jobNameLineEdit->setFocus();
    }
    else
    {
        _ui->stackedWidget->setCurrentWidget(_ui->jobDetailPage);
        _ui->jobNameLabel->setText(_job->name());
        _ui->detailTreeWidget->reset();
        _ui->detailTreeWidget->setSelectedUrls(_job->urls());
        _ui->restoreListWidget->clear();
        _ui->restoreListWidget->addArchives(_job->archives());
        connect(_ui->detailTreeWidget, SIGNAL(selectionChanged()), this, SLOT(save()));
    }
}

