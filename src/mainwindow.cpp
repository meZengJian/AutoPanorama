#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "env.h"

#include <QtDebug>
#include <QMessageBox>
#include <QFileDialog>
#include <QProgressBar>
#include <QLineEdit>
#include <QFileInfo>
#include <QToolTip>
#include <QString>
#include <QCloseEvent>

#include <opencv2/core/ocl.hpp>
#include <opencv2/core.hpp>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    manual_output_filename(false),
    manual_output_dir(false),
    max_filename_length(50)
{
    qRegisterMetaType<QVector<int>>();
    ui->setupUi(this);

    updateOutputDirFilename();
    onFastSettingsChanged();
    onBlenderTypeChange();
    onExposureCompensatorChange();
    updateOCL();
    updateEigen();
    updateIPP();
    updateArch();
    updateStatusBar();
    updateMakeEnabled();
}

MainWindow::~MainWindow()
{
    delete ui;
}

int MainWindow::getNbQueued()
{
    int nb = 0;
    for (int i = 0; i < workers.size(); ++i)
    {
        if (!workers[i]) continue;
        PanoramaMaker::Status status = workers[i]->getStatus();
        if (status == PanoramaMaker::STOPPED || status == PanoramaMaker::WORKING)
                ++nb;
    }
    return nb;
}

int MainWindow::getNbDone()
{
    int nb = 0;
    for (int i = 0; i < workers.size(); ++i)
    {
        if (!workers[i]) continue;
        PanoramaMaker::Status status = workers[i]->getStatus();
        if (status == PanoramaMaker::DONE)
                ++nb;
    }
    return nb;
}

int MainWindow::getNbFailed()
{
    int nb = 0;
    for (int i = 0; i < workers.size(); ++i)
    {
        if (!workers[i]) continue;
        PanoramaMaker::Status status = workers[i]->getStatus();
        if (status == PanoramaMaker::FAILED)
                ++nb;
    }
    return nb;
}

int MainWindow::getCurrentProgress()
{
    for (int i = 0; i < workers.size(); ++i)
    {
        if (workers[i]->getStatus() == PanoramaMaker::WORKING)
            return workers[i]->getProgress();
    }
    return 0;
}

void MainWindow::onMakePanoramaClicked()
{
    QStringList files;
    bool clear_files = false;
    if (ui->selectedOnly_checkbox->isChecked())
    {
        files = ui->filesListWidget->getSelectedFilesList();
    }
    else
    {
        files = ui->filesListWidget->getFilesList();
        clear_files = true;
    }

    QStringList supported_image_ext = PanoramaMaker::getSupportedImageExtensions();
    QStringList supported_video_ext = PanoramaMaker::getSupportedVideoExtensions();
    QStringList images, videos;

    for (const QString& file : files)
    {
        QString ext = QFileInfo(file).suffix();
        if (supported_image_ext.contains(ext))
            images.append(file);
        else if (supported_video_ext.contains(ext))
            videos.append(file);
        else
        {
            QMessageBox::warning(this, "Invalid files", "File "+file+"is not supported");
            return;
        }
    }

    if (images.size() < 2 && videos.size() < 1)
    {
        QMessageBox::warning(this, "Not enough files", "Please select at least 2 images or 1 video");
        return;
    }

    PanoramaMaker *worker = new PanoramaMaker;

    if (images.size() > 0)
    {
        try {
            worker->setImages(images);
        } catch (const invalid_argument& e) {
            QMessageBox::warning(this, "Invalid files", e.what());
            return;
        }
    }
    if (videos.size() > 0)
    {
        try {
            worker->setVideos(videos);
        } catch (const invalid_argument& e) {
            QMessageBox::warning(this, "Invalid files", e.what());
            return;
        }
    }

    worker->setOutput(ui->output_filename_lineedit->text(),
                      ui->extension_combobox->currentText(),
                      ui->output_dir_lineedit->text());
    configureWorker(worker);
    createWorkerUi(worker);
    connect(worker, SIGNAL(finished()), this, SLOT(runWorkers()));

    qDebug() << "Queuing worker";
    workers << worker;
    runWorkers();

    if (clear_files)
        ui->filesListWidget->clear();
    updateStatusBar();
}

void MainWindow::runWorkers()
{
    for (int i = 0; i < workers.size(); ++i)
    {
        if (workers[i]->isRunning())
            break;
        if (workers[i]->getStatus() == PanoramaMaker::STOPPED)
        {
            startWorker(workers[i]);
            break;
        }
    }
}

void MainWindow::onWorkerFailed(QString msg)
{
    PanoramaMaker* sender = qobject_cast<PanoramaMaker*>(QObject::sender());
    QProgressBar *pb = progress_bars[sender].pb;
    if (!pb)
        return;
    pb->setFormat(QString("%1 : Failed (%2)").arg(sender->getOutputFilename()).arg(msg));
    pb->setValue(100);
    pb->setStyleSheet("QProgressBar::chunk{background-color:red}");
    progress_bars[sender].close->setText("Hide");
    progress_bars[sender].close->setEnabled(true);
}

void MainWindow::onWorkerDone()
{
    PanoramaMaker* sender = qobject_cast<PanoramaMaker*>(QObject::sender());
    QProgressBar *pb = progress_bars[sender].pb;
    if (!pb)
        return;
    pb->setStyleSheet("QProgressBar::chunk{background-color:green}");
    pb->setFormat(QString("%1 : Done ! (%2s total, %3s processing)")
                  .arg(sender->getOutputFilename())
                  .arg(QString::number(sender->getTotalTime(), 'f', 1))
                  .arg(QString::number(sender->getProcTime(), 'f', 1)));
    progress_bars[sender].close->setText("Hide");
    progress_bars[sender].close->setEnabled(true);
}

void MainWindow::onBlenderTypeChange()
{
    QString type = ui->blendertype_combobox->currentText();
    if (type == QString("Multiband"))
    {
        ui->sharpness_label->hide();
        ui->sharpness_spinbox->hide();
        ui->nbands_label->show();
        ui->nbands_spinbox->show();
    }
    else if (type == QString("Feather"))
    {
        ui->sharpness_label->show();
        ui->sharpness_spinbox->show();
        ui->nbands_label->hide();
        ui->nbands_spinbox->hide();
    }
    else
    {
        ui->sharpness_label->hide();
        ui->sharpness_spinbox->hide();
        ui->nbands_label->hide();
        ui->nbands_spinbox->hide();
    }
}

void MainWindow::onExposureCompensatorChange()
{
    QString mode = ui->expcomp_mode_combobox->currentText();
    if (mode == QString("Blocks") ||
        mode == QString("Combined"))
    {
        ui->blocksize_label->show();
        ui->blocksize_spinbox->show();
        ui->exp_sim_th_spinbox->setValue(0.1);
    }
    else
    {
        ui->blocksize_label->hide();
        ui->blocksize_spinbox->hide();
        ui->exp_sim_th_spinbox->setValue(1);
    }
    if (mode == QString("None"))
    {
        ui->nfeed_label->hide();
        ui->nfeed_spinbox->hide();
        ui->expcomp_type_combobox->hide();
        ui->expcomp_type_label->hide();
        ui->exp_sim_th_label->hide();
        ui->exp_sim_th_spinbox->hide();
    }
    else
    {
        ui->nfeed_label->show();
        ui->nfeed_spinbox->show();
        ui->expcomp_type_combobox->show();
        ui->expcomp_type_label->show();
        ui->exp_sim_th_label->show();
        ui->exp_sim_th_spinbox->show();
    }
}

void MainWindow::onOutputFilenameChanged(QString /*edit*/)
{
    updateMakeEnabled();
}

void MainWindow::onOutputDirChanged(QString edit)
{
    QPalette palette;
    QString tooltip;

    if (edit.isEmpty() || !QDir(edit).exists())
    {
        palette.setColor(QPalette::Text,Qt::red);
        tooltip = "This directory doesn't exists";
    }
    else
    {
        palette.setColor(QPalette::Text,Qt::black);
    }
    qobject_cast<QLineEdit*>(sender())->setPalette(palette);
    qobject_cast<QLineEdit*>(sender())->setToolTip(tooltip);
    updateMakeEnabled();
}

void MainWindow::onOutputFilenameEdit(QString edit)
{
    if (edit.isEmpty())
        manual_output_filename = false;
    else
        manual_output_filename = true;
}

void MainWindow::onOutputDirEdit(QString edit)
{
    if (edit.isEmpty())
        manual_output_dir = false;
    else
        manual_output_dir = true;
}

void MainWindow::onSelectOutputDirClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
                this,
                "Select output directory",
                ui->output_dir_lineedit->text());
    if (!dir.isEmpty())
    {
        ui->output_dir_lineedit->setText(dir);
        manual_output_dir = true;
    }
    else
    {
        manual_output_dir = false;
        updateOutputDirFilename();
    }
}

void MainWindow::onFastSettingsChanged()
{
    resetAlgoSetting();
    int exp_comp = ui->fast_excomp_value->value();
    switch(exp_comp)
    {
    case 0: // Very fast
        ui->nfeed_spinbox->setValue(1);
        ui->expcomp_mode_combobox->setCurrentText("Simple");
        break;
    case 1: // Fast
        ui->nfeed_spinbox->setValue(3);
        ui->expcomp_mode_combobox->setCurrentText("Simple");
        break;
    case 2: // Slow
        ui->nfeed_spinbox->setValue(1);
        ui->expcomp_mode_combobox->setCurrentText("Combined");
        break;
    case 3: // Very slow
        ui->nfeed_spinbox->setValue(3);
        ui->expcomp_mode_combobox->setCurrentText("Combined");
        break;
    }

    int pan_size = ui->fast_pan_size_value->value();
    switch(pan_size)
    {
    case 0: // Small
        ui->compositingres_spinbox->setValue(1);
        break;
    case 1: // Medium
        ui->compositingres_spinbox->setValue(5);
        break;
    case 2: // Full size
        ui->compositingres_spinbox->setValue(0);
        break;
    }

    QString proj_type = "Spherical";
    if (ui->fast_proj_type_sph->isChecked())
        proj_type = "Spherical";
    else if (ui->fast_proj_type_cyl->isChecked())
        proj_type = "Cylindrical";
    else if (ui->fast_proj_type_pla->isChecked())
        proj_type = "Perspective";
    ui->warpmode_combobox->setCurrentText(proj_type);

}

void MainWindow::resetAlgoSetting()
{
    ui->regres_spinbox->setValue(0.6);
    ui->featuresfinder_combobox->setCurrentText("AKAZE");
    ui->featuresmatcher_combobox->setCurrentText("Best of 2 nearest");
    ui->featuresmatcherconf_spinbox->setValue(0.65);
    ui->warpmode_combobox->setCurrentText("Spherical");
    ui->wavecorkind_combobox->setCurrentText("None");
    ui->bundleadj_combobox->setCurrentText("Ray");
    ui->confth_spinbox->setValue(1.0);

    ui->expcomp_mode_combobox->setCurrentText("Combined");
    ui->expcomp_type_combobox->setCurrentText("BGR");
    ui->nfeed_spinbox->setValue(3);
    ui->blocksize_spinbox->setValue(32);
    ui->exp_sim_th_spinbox->setValue(0.1);

    ui->seamfinderres_spinbox->setValue(0.1);
    ui->seamfindermode_combobox->setCurrentText("Graph cut color");

    ui->blendertype_combobox->setCurrentText("Multiband");
    ui->nbands_spinbox->setValue(3);

    ui->compositingres_spinbox->setValue(0);
    ui->interp_combobox->setCurrentText("Cubic");
}

void MainWindow::updateMakeEnabled()
{
    QString filename = ui->output_filename_lineedit->text();
    QString dir = ui->output_dir_lineedit->text();
    bool enabled = ui->buttonMakePanorama->isEnabled();

    if (enabled)
    {
        if (filename.isEmpty())
        {
            ui->buttonMakePanorama->setEnabled(false);
            return;
        }

        if (dir.isEmpty() || !QDir(dir).exists())
        {
            ui->buttonMakePanorama->setEnabled(false);
            return;
        }
    }
    else if (!filename.isEmpty() && !dir.isEmpty() && QDir(dir).exists())
        ui->buttonMakePanorama->setEnabled(true);
}

void MainWindow::updateOCL()
{
    QString yes("Yes"), no("No");
    bool have_opencl = cv::ocl::haveOpenCL();
    ui->haveopencl_value->setText(have_opencl ? yes : no);
    ui->use_opencl_checkbox->setEnabled(have_opencl);
    ui->use_opencl_checkbox->setChecked(false);

    if (!have_opencl)
    {
        ui->opencl_device_tree->hide();
        ui->opencl_device_tree_label->hide();
    }
    else
    {
        ocl::Device default_device = ocl::Device::getDefault();
        std::vector<cv::ocl::PlatformInfo> platforms;
        cv::ocl::getPlatfomsInfo(platforms);
        for (size_t i = 0; i < platforms.size(); i++)
        {
            const cv::ocl::PlatformInfo* platform = &platforms[i];

            QString platform_txt = QString::fromStdString(platform->name());
            QTreeWidgetItem *platform_item = new QTreeWidgetItem(QStringList(platform_txt));
            ui->opencl_device_tree->insertTopLevelItem(i, platform_item);

            cv::ocl::Device current_device;
            for (int j = 0; j < platform->deviceNumber(); j++)
            {
                platform->getDevice(current_device, j);

                QString device_txt;
                device_txt = QString::fromStdString(current_device.name());
                device_txt += " ("+oclDeviceTypeToString(current_device.type())+")";

                QTreeWidgetItem *device_item = new QTreeWidgetItem(QStringList(device_txt));
                platform_item->addChild(device_item);
                if (!current_device.available())
                {
                    device_item->setDisabled(true);
                }

                if (current_device.name() == default_device.name())
                {
                    device_item->setForeground(0, QBrush(Qt::green));
                }
                QString mem_type;
                if (current_device.localMemType() == ocl::Device::LOCAL_IS_GLOBAL)
                    mem_type = "global";
                else if (current_device.localMemType() == ocl::Device::LOCAL_IS_LOCAL)
                    mem_type = "local";
                else if (current_device.localMemType() == ocl::Device::NO_LOCAL_MEM)
                    mem_type = "none";
                QString tooltip;
                tooltip += QString("2D Image max size : %1x%2\n").arg(current_device.image2DMaxWidth()).arg(current_device.image2DMaxWidth());
                tooltip += QString("Max mem alloc size : %1 MB\n").arg(current_device.maxMemAllocSize()/1000000);
                tooltip += QString("Global mem size : %1 MB\n").arg(current_device.globalMemSize()/1000000);
                tooltip += QString("Local mem type and size : %1 kB (%2)").arg(current_device.localMemSize()/1000).arg(mem_type);
                device_item->setToolTip(0, tooltip);
            }
        }
        ui->opencl_device_tree->expandAll();
        ui->opencl_device_tree->show();
        ui->opencl_device_tree_label->show();
    }
}

void MainWindow::updateEigen()
{
    QRegExp regex("Use Eigen:([ \\t]*)([^\\n\\r]*)");
    regex.indexIn(cv::getBuildInformation().c_str());
    ui->have_eigen_value->setText(regex.cap(2).replace("YES", "Yes").replace("NO", "No"));
}

void MainWindow::updateIPP()
{
    QRegExp regex("Use IPP:([ \\t]*)([^\\n\\r]*)");
    regex.indexIn(cv::getBuildInformation().c_str());
    ui->have_ipp_value->setText(regex.cap(2).replace("YES", "Yes").replace("NO", "No"));
}

void MainWindow::updateArch()
{
#ifdef ENVIRONMENT64
    ui->arch_value->setText("64 bits");
#else
#ifdef ENVIRONMENT32
    ui->arch_value->setText("32 bits");
#endif
#endif
}

void MainWindow::updateOutputDirFilename()
{
    QStringList fl;
    if (ui->selectedOnly_checkbox->isChecked())
        fl = ui->filesListWidget->getSelectedFilesList();
    else
        fl = ui->filesListWidget->getFilesList();

    if (!manual_output_dir)
    {
        if (fl.size() > 0)
            ui->output_dir_lineedit->setText(QFileInfo(fl[0]).absoluteDir().path());
        else
            ui->output_dir_lineedit->setText("");
    }

    if (!manual_output_filename)
    {
        if (fl.size() > 0)
        {
            QStringList basenames;
            QString new_name;
            for (int i = 0; i < fl.size(); ++i)
                basenames << QFileInfo(fl[i]).baseName();
            new_name = "pano_"+basenames.join("_");
            new_name.truncate(max_filename_length);
            ui->output_filename_lineedit->setText(new_name);
        }
        else
            ui->output_filename_lineedit->setText("");
    }
}

void MainWindow::updateStatusBar()
{
    QString text = QString("Current job : %1%       Jobs left : %2      Jobs done : %3      Jobs failed : %4")
            .arg(getCurrentProgress()).arg(getNbQueued()).arg(getNbDone()).arg(getNbFailed());
    ui->statusBar->showMessage(text);
}








void MainWindow::closeEvent(QCloseEvent *event)
{
    int nb = getNbQueued();
    if (nb == 0)
        close();
    else
    {
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "Quit ?",
                                      QString("All panoramas are not done yet. \nAre you sure you want to quit ?"),
                                      QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes)
            close();
        else
            event->ignore();
    }

}








void MainWindow::startWorker(PanoramaMaker *worker)
{
    qDebug() << "Starting worker";
    QPushButton *close = progress_bars[worker].close;
    close->setDisabled(true);
    worker->start();
}

void MainWindow::createWorkerUi(PanoramaMaker *worker) {
    QProgressBar *progress_bar = new QProgressBar;
    QHBoxLayout *hbox = new QHBoxLayout;
    QPushButton *close;
    close = new QPushButton("Cancel");

    progress_bar->setRange(0,100);
    progress_bar->setFormat(worker->getOutputFilename()+" : %p%");
    progress_bar->setAlignment(Qt::AlignCenter);
    progress_bar->setValue(0);
    progress_bar->setToolTip(worker->getStitcherConfString());

    ProgressBarContent pb_struct;
    pb_struct.pb = progress_bar;
    pb_struct.close = close;
    pb_struct.worker = worker;
    pb_struct.layout = hbox;

    progress_bars[worker] = pb_struct;
    progress_bars[close] = pb_struct;

    connect(worker, SIGNAL(percentage(int)), progress_bar, SLOT(setValue(int)));
    connect(worker, SIGNAL(percentage(int)), this, SLOT(updateStatusBar()));
    connect(worker, SIGNAL(is_failed(QString)), this, SLOT(onWorkerFailed(QString)));
    connect(worker, SIGNAL(is_done()), this, SLOT(onWorkerDone()));
    connect(worker, SIGNAL(finished()), this, SLOT(updateStatusBar()));

    connect(close, SIGNAL(clicked(bool)), this, SLOT(closeSenderWorker()));

    hbox->addWidget(progress_bar);
    hbox->addWidget(close);
    ui->tabProgressLayout->addLayout(hbox);
}

void MainWindow::configureWorker(PanoramaMaker *worker)
{
    // OpenCL
    worker->setUseOpenCL(ui->use_opencl_checkbox->isChecked());

    // Registration resolution
    worker->setRegistrationResol(ui->regres_spinbox->value());

    // Feature finder mode
    worker->setFeaturesFinderMode(ui->featuresfinder_combobox->currentText());

    // Feature matching mode and confidence
    PanoramaMaker::FeaturesMatchingMode f_matching_mode;
    f_matching_mode.mode = ui->featuresmatcher_combobox->currentText();
    f_matching_mode.conf = ui->featuresmatcherconf_spinbox->value();
    worker->setFeaturesMatchingMode(f_matching_mode);

    // Warp mode
    worker->setWarpMode(ui->warpmode_combobox->currentText());

    // Wave correction
    worker->setWaveCorrectionMode(ui->wavecorkind_combobox->currentText());

    // Bundle adjuster
    worker->setBundleAdjusterMode(ui->bundleadj_combobox->currentText());

    // Panorama confidence
    worker->setPanoConfidenceThresh(ui->confth_spinbox->value());

    // Exposure compensator mode
    PanoramaMaker::ExposureComensatorMode exp_comp_mode;
    exp_comp_mode.mode = ui->expcomp_mode_combobox->currentText();
    if (ui->expcomp_type_combobox->currentText() == QString("Gain"))
    {
        exp_comp_mode.type = detail::GainCompensator::GAIN;
    }
    else if (ui->expcomp_type_combobox->currentText() == QString("BGR"))
    {
        exp_comp_mode.type = detail::GainCompensator::CHANNELS;
    }
    exp_comp_mode.block_size = ui->blocksize_spinbox->value();
    exp_comp_mode.nfeed = ui->nfeed_spinbox->value();
    exp_comp_mode.similarity_th = ui->exp_sim_th_spinbox->value();
    worker->setExposureCompensatorMode(exp_comp_mode);

    // Seam estimation resolution
    worker->setSeamEstimationResol(ui->seamfinderres_spinbox->value());

    // Seam finder mode
    worker->setSeamFinderMode(ui->seamfindermode_combobox->currentText());

    // Blender
    PanoramaMaker::BlenderMode blender_mode;
    blender_mode.mode = ui->blendertype_combobox->currentText();
    blender_mode.sharpness = ui->sharpness_spinbox->value();
    blender_mode.bands = ui->nbands_spinbox->value();
    worker->setBlenderMode(blender_mode);

    // Compositing resolution
    double compositing_res = ui->compositingres_spinbox->value();
    if (compositing_res <= 0)
        compositing_res = Stitcher::ORIG_RESOL;

    worker->setCompositingResol(compositing_res);

    // Interpolation
    worker->setInterpolationMode(ui->interp_combobox->currentText());

    // Inner cut
    worker->setGenerateInnerCut(ui->inner_cut_checkbox->isChecked());
}

void MainWindow::closeSenderWorker()
{
    ProgressBarContent pb_struct = progress_bars[QObject::sender()];
    progress_bars.erase(pb_struct.close);
    progress_bars.erase(pb_struct.worker);
    delete pb_struct.pb;
    delete pb_struct.close;
    delete pb_struct.layout;
    workers.removeAll(pb_struct.worker);
    if (pb_struct.worker)
    {
        delete pb_struct.worker;
    }
    updateStatusBar();
}

QString MainWindow::oclDeviceTypeToString(int type)
{
    QStringList strs;
    if (type & ocl::Device::TYPE_CPU)
        strs << "CPU";
    if (type & ocl::Device::TYPE_GPU)
    {
        bool type_found = false;
        if ((type-ocl::Device::TYPE_GPU) & ocl::Device::TYPE_DGPU)
        {
            type_found = true;
            strs << "DGPU";
        }
        if ((type-ocl::Device::TYPE_GPU) & ocl::Device::TYPE_IGPU)
        {
            type_found = true;
            strs << "IGPU";
        }
        if (!type_found)
        {
            strs << "GPU";
        }
    }
    if (type & ocl::Device::TYPE_ACCELERATOR)
        strs << "Accelerator";


    if (strs.size() == 0)
        return QString("Unknown type");

    return strs.join(", ");
}
