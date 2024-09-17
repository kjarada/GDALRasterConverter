// main.cpp

#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QLineEdit>
#include <QProgressBar>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QTextEdit>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QComboBox>
#include <QMap>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QThreadPool>
#include <QRunnable>
#include <atomic>
#include <memory>
#include <optional>
#include <iostream>

// GDAL Headers
#include "gdal_priv.h"
#include "cpl_conv.h" // for CPLMalloc()

// Worker class to handle conversion in a separate thread
class Worker : public QObject
{
    Q_OBJECT

public:
    enum ProcessingMode { CPU, GPU };
    Q_ENUM(ProcessingMode)

    Worker(QString inputPath, QString outputPath, QString inputDriverName, QString outputDriverName, QMap<QString, QString> options, ProcessingMode mode, int numCores)
        : inputFile(std::move(inputPath)), outputFile(std::move(outputPath)),
          inputDriverName(std::move(inputDriverName)), outputDriverName(std::move(outputDriverName)),
          gdalOptions(std::move(options)), isConverting(true), processingMode(mode), numCores(numCores) {}

    ~Worker() override = default;

public slots:
    void process()
    {
        emit logMessage("Starting GDAL conversion...");

        // Open the input file
        GDALDataset* poDataset = static_cast<GDALDataset*>(GDALOpenEx(
            inputFile.toStdString().c_str(), GDAL_OF_READONLY, nullptr, nullptr, nullptr));

        if (!poDataset)
        {
            QString errorMsg = "Failed to open input file: " + inputFile + "\nGDAL Error: " + QString(CPLGetLastErrorMsg());
            emit finished(false, errorMsg);
            return;
        }

        emit logMessage("Input file opened successfully.");

        // Get the output driver
        GDALDriver* poOutDriver = GetGDALDriverManager()->GetDriverByName(outputDriverName.toStdString().c_str());
        if (!poOutDriver)
        {
            QString errorMsg = "Output driver not available: " + outputDriverName;
            GDALClose(poDataset);
            emit finished(false, errorMsg);
            return;
        }

        emit logMessage("Output driver found: " + outputDriverName);

        // Set creation options for the output file
        char** papszOptions = nullptr;
        for (auto it = gdalOptions.begin(); it != gdalOptions.end(); ++it)
        {
            papszOptions = CSLSetNameValue(papszOptions, it.key().toStdString().c_str(), it.value().toStdString().c_str());
            emit logMessage(QString("Setting GDAL option: %1 = %2").arg(it.key(), it.value()));
        }

        // Check if driver supports Create method
        bool bCreateSupported = poOutDriver->GetMetadataItem(GDAL_DCAP_CREATE) != nullptr;

        if (processingMode == CPU)
        {
            emit logMessage("Processing mode: CPU");

            if (bCreateSupported)
            {
                // Proceed with Create method
                if (!processWithCreateMethod(poDataset, poOutDriver, papszOptions))
                {
                    GDALClose(poDataset);
                    CSLDestroy(papszOptions);
                    return;
                }
            }
            else if (poOutDriver->GetMetadataItem(GDAL_DCAP_CREATECOPY) != nullptr)
            {
                // Use CreateCopy method
                if (!processWithCreateCopyMethod(poDataset, poOutDriver, papszOptions))
                {
                    GDALClose(poDataset);
                    CSLDestroy(papszOptions);
                    return;
                }
            }
            else
            {
                QString errorMsg = "Output driver does not support Create or CreateCopy methods.";
                GDALClose(poDataset);
                CSLDestroy(papszOptions);
                emit finished(false, errorMsg);
                return;
            }

            GDALClose(poDataset);
            CSLDestroy(papszOptions);

            emit logMessage("Conversion process completed successfully.");
            emit finished(true, "Conversion completed successfully: " + outputFile);
        }
        else if (processingMode == GPU)
        {
            emit logMessage("Processing mode: GPU");

            // Placeholder for GPU processing code
            GDALClose(poDataset);
            CSLDestroy(papszOptions);

            emit logMessage("GPU processing is not yet implemented.");
            emit finished(false, "GPU processing is not yet implemented.");
            return;
        }
        else
        {
            // Unknown processing mode
            GDALClose(poDataset);
            CSLDestroy(papszOptions);
            emit finished(false, "Unknown processing mode selected.");
            return;
        }
    }

    void requestInterruption()
    {
        isConverting.store(false, std::memory_order_relaxed);
    }

signals:
    void progressUpdated(float progress);
    void finished(bool success, const QString &message);
    void logMessage(const QString &message);

private:
    bool processWithCreateMethod(GDALDataset* poDataset, GDALDriver* poOutDriver, char** papszOptions)
    {
        emit logMessage("Using Create method.");

        // Get input dataset dimensions and properties
        int nBands = poDataset->GetRasterCount();
        if (nBands == 0)
        {
            QString errorMsg = "Input dataset has no raster bands.";
            emit finished(false, errorMsg);
            return false;
        }

        int nXSize = poDataset->GetRasterXSize();
        int nYSize = poDataset->GetRasterYSize();
        GDALDataType eType = poDataset->GetRasterBand(1)->GetRasterDataType();

        // Create output dataset
        GDALDataset* poOutDataset = poOutDriver->Create(
            outputFile.toStdString().c_str(),
            nXSize,
            nYSize,
            nBands,
            eType,
            papszOptions);

        if (!poOutDataset)
        {
            QString errorMsg = "Failed to create output dataset: " + outputFile + "\nGDAL Error: " + QString(CPLGetLastErrorMsg());
            emit finished(false, errorMsg);
            return false;
        }

        // Copy projection and geotransform
        const char* projection = poDataset->GetProjectionRef();
        if (projection)
        {
            poOutDataset->SetProjection(projection);
        }

        double geotransform[6];
        if (poDataset->GetGeoTransform(geotransform) == CE_None)
        {
            poOutDataset->SetGeoTransform(geotransform);
        }

        // Processing and writing data
        if (!processData(poDataset, poOutDataset))
        {
            GDALClose(poOutDataset);
            return false;
        }

        GDALClose(poOutDataset);
        return true;
    }

    bool processWithCreateCopyMethod(GDALDataset* poDataset, GDALDriver* poOutDriver, char** papszOptions)
    {
        emit logMessage("Using CreateCopy method.");

        // Copy the dataset directly
        GDALDataset* poOutDataset = poOutDriver->CreateCopy(
            outputFile.toStdString().c_str(),
            poDataset,
            FALSE, // Synchronous copy
            papszOptions,
            progressCallback,
            this);

        if (!poOutDataset)
        {
            QString errorMsg = "Failed to create output dataset using CreateCopy: " + outputFile + "\nGDAL Error: " + QString(CPLGetLastErrorMsg());
            emit finished(false, errorMsg);
            return false;
        }

        // Close the output dataset
        GDALClose(poOutDataset);

        return true;
    }

    bool processData(GDALDataset* poDataset, GDALDataset* poOutDataset)
    {
        // Block processing as before
        int nXSize = poDataset->GetRasterXSize();
        int nYSize = poDataset->GetRasterYSize();
        int nBands = poDataset->GetRasterCount();

        int blockSizeX = 256;
        int blockSizeY = 256;

        int nXBlocks = (nXSize + blockSizeX - 1) / blockSizeX;
        int nYBlocks = (nYSize + blockSizeY - 1) / blockSizeY;

        int totalBlocks = nXBlocks * nYBlocks;
        std::atomic<int> blocksCompleted(0);

        // Thread pool
        QThreadPool threadPool;
        threadPool.setMaxThreadCount(numCores);

        // Process blocks
        emit logMessage(QString("Starting block processing using %1 core(s)...").arg(numCores));

        for (int y = 0; y < nYSize && isConverting.load(); y += blockSizeY)
        {
            int nYBlockSize = std::min(blockSizeY, nYSize - y);
            for (int x = 0; x < nXSize && isConverting.load(); x += blockSizeX)
            {
                int nXBlockSize = std::min(blockSizeX, nXSize - x);

                // Read data in the main thread
                std::vector<std::vector<char>> bandData(nBands);
                std::vector<GDALDataType> bandTypes(nBands);

                for (int bandIndex = 1; bandIndex <= nBands; ++bandIndex)
                {
                    GDALRasterBand* poBand = poDataset->GetRasterBand(bandIndex);
                    int nPixels = nXBlockSize * nYBlockSize;
                    GDALDataType eType = poBand->GetRasterDataType();
                    bandTypes[bandIndex - 1] = eType;
                    int nBytes = GDALGetDataTypeSizeBytes(eType) * nPixels;

                    bandData[bandIndex - 1].resize(nBytes);

                    CPLErr err = poBand->RasterIO(GF_Read, x, y, nXBlockSize, nYBlockSize, bandData[bandIndex - 1].data(), nXBlockSize, nYBlockSize, eType, 0, 0, nullptr);

                    if (err != CE_None)
                    {
                        QString errorMsg = "Failed to read data from input dataset.\nGDAL Error: " + QString(CPLGetLastErrorMsg());
                        emit finished(false, errorMsg);
                        return false;
                    }
                }

                // Process data in worker threads
                class BlockProcessor : public QRunnable
                {
                public:
                    BlockProcessor(std::vector<std::vector<char>>& bandData, int nXBlockSize, int nYBlockSize, int nBands, std::atomic<bool>* isConverting)
                        : bandData(bandData), nXBlockSize(nXBlockSize), nYBlockSize(nYBlockSize), nBands(nBands), isConverting(isConverting)
                    {
                        setAutoDelete(true);
                    }

                    void run() override
                    {
                        if (!isConverting->load())
                            return;

                        // Placeholder for data processing
                        // Add custom processing logic here if needed
                    }

                private:
                    std::vector<std::vector<char>>& bandData;
                    int nXBlockSize;
                    int nYBlockSize;
                    int nBands;
                    std::atomic<bool>* isConverting;
                };

                // Create and start the task
                BlockProcessor* task = new BlockProcessor(bandData, nXBlockSize, nYBlockSize, nBands, &isConverting);
                threadPool.start(task);

                if (!isConverting.load())
                {
                    // Conversion was cancelled
                    threadPool.waitForDone();
                    emit finished(false, "Conversion cancelled by user.");
                    return false;
                }

                // Wait for the task to complete
                threadPool.waitForDone();

                // Write data back to the output dataset in the main thread
                for (int bandIndex = 1; bandIndex <= nBands; ++bandIndex)
                {
                    GDALRasterBand* poOutBand = poOutDataset->GetRasterBand(bandIndex);
                    GDALDataType eType = bandTypes[bandIndex - 1];

                    CPLErr err = poOutBand->RasterIO(GF_Write, x, y, nXBlockSize, nYBlockSize, bandData[bandIndex - 1].data(), nXBlockSize, nYBlockSize, eType, 0, 0, nullptr);

                    if (err != CE_None)
                    {
                        QString errorMsg = "Failed to write data to output dataset.\nGDAL Error: " + QString(CPLGetLastErrorMsg());
                        emit finished(false, errorMsg);
                        return false;
                    }
                }

                // Update progress
                blocksCompleted.fetch_add(1, std::memory_order_relaxed);
                float progress = static_cast<float>(blocksCompleted.load()) / totalBlocks;
                emit progressUpdated(progress);
            }
        }

        if (!isConverting.load())
        {
            // Conversion was cancelled
            emit finished(false, "Conversion cancelled by user.");
            return false;
        }

        // Final progress update
        emit progressUpdated(1.0f);

        return true;
    }

    static int progressCallback(double dfComplete, const char* pszMessage, void* pProgressArg)
    {
        Worker* worker = static_cast<Worker*>(pProgressArg);
        if (worker->isConverting.load())
        {
            worker->emit progressUpdated(static_cast<float>(dfComplete));
            return TRUE; // Continue processing
        }
        else
        {
            return FALSE; // Cancel processing
        }
    }

    QString inputFile;
    QString outputFile;
    QString inputDriverName;
    QString outputDriverName;
    QMap<QString, QString> gdalOptions;
    std::atomic<bool> isConverting;
    ProcessingMode processingMode;
    int numCores;
};

// Main Window class
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr)
        : QMainWindow(parent), worker(nullptr), thread(nullptr)
    {
        // Set Window Title
        setWindowTitle("GDAL Raster Data Converter");

        // Central Widget
        QWidget *centralWidget = new QWidget(this);
        setCentralWidget(centralWidget);

        // Main Layout
        QVBoxLayout *mainLayout = new QVBoxLayout();
        centralWidget->setLayout(mainLayout);

        // Input File Selection
        QHBoxLayout *inputLayout = new QHBoxLayout();
        QLabel *inputLabel = new QLabel("Input File:");
        inputLineEdit = new QLineEdit();
        QPushButton *browseInputButton = new QPushButton("Browse...");
        inputLayout->addWidget(inputLabel);
        inputLayout->addWidget(inputLineEdit);
        inputLayout->addWidget(browseInputButton);
        mainLayout->addLayout(inputLayout);

        // Input Driver Selection
        QHBoxLayout *inputDriverLayout = new QHBoxLayout();
        QLabel *inputDriverLabel = new QLabel("Input Format:");
        inputDriverComboBox = new QComboBox();
        inputDriverLayout->addWidget(inputDriverLabel);
        inputDriverLayout->addWidget(inputDriverComboBox);
        mainLayout->addLayout(inputDriverLayout);

        // Output File Selection
        QHBoxLayout *outputLayout = new QHBoxLayout();
        QLabel *outputLabel = new QLabel("Output File:");
        outputLineEdit = new QLineEdit();
        QPushButton *browseOutputButton = new QPushButton("Browse...");
        outputLayout->addWidget(outputLabel);
        outputLayout->addWidget(outputLineEdit);
        outputLayout->addWidget(browseOutputButton);
        mainLayout->addLayout(outputLayout);

        // Output Driver Selection
        QHBoxLayout *outputDriverLayout = new QHBoxLayout();
        QLabel *outputDriverLabel = new QLabel("Output Format:");
        outputDriverComboBox = new QComboBox();
        outputDriverLayout->addWidget(outputDriverLabel);
        outputDriverLayout->addWidget(outputDriverComboBox);
        mainLayout->addLayout(outputDriverLayout);

        // Use GDAL Options Checkbox
        useOptionsCheckBox = new QCheckBox("Use GDAL Options");
        useOptionsCheckBox->setChecked(false); // Default is not to use GDAL options
        mainLayout->addWidget(useOptionsCheckBox);

        // GDAL Options Group
        optionsGroup = new QGroupBox("GDAL Creation Options");
        optionsLayout = new QVBoxLayout();
        optionsGroup->setLayout(optionsLayout);
        optionsGroup->setEnabled(false); // Initially disabled
        mainLayout->addWidget(optionsGroup);

        // Processing Mode Selection
        QGroupBox* processingModeGroup = new QGroupBox("Processing Mode");
        QHBoxLayout* processingModeLayout = new QHBoxLayout();
        cpuRadioButton = new QRadioButton("CPU");
        gpuRadioButton = new QRadioButton("GPU");
        cpuRadioButton->setChecked(true); // Default to CPU
        processingModeLayout->addWidget(cpuRadioButton);
        processingModeLayout->addWidget(gpuRadioButton);
        processingModeGroup->setLayout(processingModeLayout);
        mainLayout->addWidget(processingModeGroup);

        // Number of CPU Cores Selection
        QHBoxLayout* cpuCoresLayout = new QHBoxLayout();
        QLabel* cpuCoresLabel = new QLabel("Number of CPU Cores:");
        cpuCoresSpinBox = new QSpinBox();
        int maxCores = QThread::idealThreadCount();
        cpuCoresSpinBox->setRange(1, maxCores);
        cpuCoresSpinBox->setValue(maxCores); // Default to maximum available cores
        cpuCoresLayout->addWidget(cpuCoresLabel);
        cpuCoresLayout->addWidget(cpuCoresSpinBox);
        mainLayout->addLayout(cpuCoresLayout);

        // Start and Cancel Buttons
        QHBoxLayout *buttonLayout = new QHBoxLayout();
        startButton = new QPushButton("Start Conversion");
        cancelButton = new QPushButton("Cancel");
        cancelButton->setEnabled(false); // Initially disabled
        buttonLayout->addWidget(startButton);
        buttonLayout->addWidget(cancelButton);
        mainLayout->addLayout(buttonLayout);

        // Progress Bar and ETA
        QHBoxLayout *progressLayout = new QHBoxLayout();
        progressBar = new QProgressBar();
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        etaLabel = new QLabel("ETA: N/A");
        progressLayout->addWidget(progressBar);
        progressLayout->addWidget(etaLabel);
        mainLayout->addLayout(progressLayout);

        // Log Window
        QLabel *logLabel = new QLabel("Log Output:");
        mainLayout->addWidget(logLabel);

        logWindow = new QTextEdit();
        logWindow->setReadOnly(true);
        logWindow->setFixedHeight(150);
        mainLayout->addWidget(logWindow);

        // Connect Signals and Slots
        connect(browseInputButton, &QPushButton::clicked, this, &MainWindow::browseInputFile);
        connect(browseOutputButton, &QPushButton::clicked, this, &MainWindow::browseOutputFile);
        connect(startButton, &QPushButton::clicked, this, &MainWindow::startConversion);
        connect(cancelButton, &QPushButton::clicked, this, &MainWindow::cancelConversion);

        // Adjusted connect statements using lambda functions
        connect(outputDriverComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int){ updateOutputFileExtension(); });
        connect(inputLineEdit, &QLineEdit::textChanged,
                this, [this](const QString &){ updateOutputFileExtension(); });

        connect(useOptionsCheckBox, &QCheckBox::toggled, optionsGroup, &QGroupBox::setEnabled);

        // Initialize Timer for ETA calculation
        timer = std::make_unique<QElapsedTimer>();

        // Initialize GDAL and populate driver lists
        initializeGDAL();

        // Update options when output driver changes
        connect(outputDriverComboBox, &QComboBox::currentTextChanged, this, &MainWindow::updateOptions);

        // Ensure CPU cores selection is only enabled in CPU mode
        connect(cpuRadioButton, &QRadioButton::toggled, this, [this](bool checked){
            cpuCoresSpinBox->setEnabled(checked);
        });
        cpuCoresSpinBox->setEnabled(cpuRadioButton->isChecked());
    }

    ~MainWindow() override
    {
        if (worker)
        {
            worker->requestInterruption();
            worker->deleteLater();
        }
        if (thread)
        {
            thread->quit();
            thread->wait();
        }
    }

private slots:
    void browseInputFile()
    {
        QString selectedFilter;
        QString fileName = QFileDialog::getOpenFileName(this, "Select Input File", "", inputFileFilter, &selectedFilter);
        if (!fileName.isEmpty())
        {
            inputLineEdit->setText(fileName);
        }
    }

    void browseOutputFile()
    {
        QString selectedFilter;
        QString fileName = QFileDialog::getSaveFileName(this, "Select Output File", "", outputFileFilter, &selectedFilter);
        if (!fileName.isEmpty())
        {
            outputLineEdit->setText(fileName);
        }
    }

    void startConversion()
    {
        QString inputPath = inputLineEdit->text();
        QString outputPath = outputLineEdit->text();
        QString inputDriverName = inputDriverComboBox->currentData().toString();
        QString outputDriverName = outputDriverComboBox->currentData().toString();

        if (inputPath.isEmpty() || outputPath.isEmpty())
        {
            QMessageBox::warning(this, "Input Required", "Please specify both input and output file paths.");
            return;
        }

        // Validate input file
        QFileInfo inputFileInfo(inputPath);
        if (!inputFileInfo.exists())
        {
            QMessageBox::warning(this, "Invalid Input File", "Please select a valid input file.");
            return;
        }

        // Collect GDAL options based on user selections
        QMap<QString, QString> options;

        if (useOptionsCheckBox->isChecked())
        {
            // Gather options from the optionsLayout
            QList<QWidget*> optionWidgets = optionsGroup->findChildren<QWidget*>();
            for (QWidget* widget : optionWidgets)
            {
                QString key = widget->property("optionKey").toString();
                if (!key.isEmpty())
                {
                    QVariant value;
                    if (QLineEdit* lineEdit = qobject_cast<QLineEdit*>(widget))
                    {
                        value = lineEdit->text();
                    }
                    else if (QCheckBox* checkBox = qobject_cast<QCheckBox*>(widget))
                    {
                        value = checkBox->isChecked() ? "YES" : "NO";
                    }
                    else if (QComboBox* comboBox = qobject_cast<QComboBox*>(widget))
                    {
                        value = comboBox->currentText();
                    }
                    else if (QSpinBox* spinBox = qobject_cast<QSpinBox*>(widget))
                    {
                        value = QString::number(spinBox->value());
                    }
                    else if (QDoubleSpinBox* doubleSpinBox = qobject_cast<QDoubleSpinBox*>(widget))
                    {
                        value = QString::number(doubleSpinBox->value());
                    }

                    if (!value.toString().isEmpty())
                    {
                        options.insert(key, value.toString());
                    }
                }
            }
        }

        // Determine processing mode
        Worker::ProcessingMode mode = cpuRadioButton->isChecked() ? Worker::CPU : Worker::GPU;

        // Get number of CPU cores to use
        int numCores = cpuCoresSpinBox->value();

        // Disable UI elements during conversion
        startButton->setEnabled(false);
        cancelButton->setEnabled(true);
        // Disable browse buttons and inputs
        inputLineEdit->setEnabled(false);
        outputLineEdit->setEnabled(false);
        inputDriverComboBox->setEnabled(false);
        outputDriverComboBox->setEnabled(false);
        QList<QPushButton*> buttons = centralWidget()->findChildren<QPushButton*>();
        foreach(QPushButton* btn, buttons)
        {
            if (btn != startButton && btn != cancelButton)
                btn->setEnabled(false);
        }
        optionsGroup->setEnabled(false);
        useOptionsCheckBox->setEnabled(false);
        cpuRadioButton->setEnabled(false);
        gpuRadioButton->setEnabled(false);
        cpuCoresSpinBox->setEnabled(false);

        // Reset progress bar and ETA
        progressBar->setValue(0);
        etaLabel->setText("ETA: Calculating...");
        logWindow->clear();

        // Start timer for ETA calculation
        timer->restart();

        // Create and start worker thread
        worker = new Worker(inputPath, outputPath, inputDriverName, outputDriverName, options, mode, numCores);
        thread = new QThread();

        worker->moveToThread(thread);

        // Connect signals and slots
        connect(thread, &QThread::started, worker, &Worker::process);
        connect(worker, &Worker::progressUpdated, this, &MainWindow::updateProgress);
        connect(worker, &Worker::finished, this, &MainWindow::conversionFinished);
        connect(worker, &Worker::logMessage, this, &MainWindow::appendLog);
        connect(worker, &Worker::finished, thread, &QThread::quit);
        connect(worker, &Worker::finished, worker, &Worker::deleteLater);
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);

        thread->start();
    }

    void cancelConversion()
    {
        if (worker)
        {
            worker->requestInterruption();
            appendLog("Cancellation requested...");
        }
        cancelButton->setEnabled(false);
    }

    void updateProgress(float progress)
    {
        progressBar->setValue(static_cast<int>(progress * 100));

        // Calculate ETA
        qint64 elapsed = timer->elapsed(); // in milliseconds
        if (progress > 0.0f && progress <= 1.0f)
        {
            qint64 estimatedTotal = static_cast<qint64>(elapsed / progress);
            qint64 estimatedRemaining = estimatedTotal - elapsed;

            int hours = static_cast<int>(estimatedRemaining / 3600000);
            int minutes = static_cast<int>((estimatedRemaining % 3600000) / 60000);
            int seconds = static_cast<int>((estimatedRemaining % 60000) / 1000);

            QString etaText = QString("ETA: %1:%2:%3")
                                  .arg(hours, 2, 10, QChar('0'))
                                  .arg(minutes, 2, 10, QChar('0'))
                                  .arg(seconds, 2, 10, QChar('0'));

            etaLabel->setText(etaText);
        }
        else
        {
            etaLabel->setText("ETA: Calculating...");
        }
    }

    void conversionFinished(bool success, const QString &message)
    {
        // Re-enable UI elements
        startButton->setEnabled(true);
        cancelButton->setEnabled(false);
        // Re-enable browse buttons and inputs
        inputLineEdit->setEnabled(true);
        outputLineEdit->setEnabled(true);
        inputDriverComboBox->setEnabled(true);
        outputDriverComboBox->setEnabled(true);
        QList<QPushButton*> buttons = centralWidget()->findChildren<QPushButton*>();
        foreach(QPushButton* btn, buttons)
        {
            if (btn != startButton && btn != cancelButton)
                btn->setEnabled(true);
        }
        optionsGroup->setEnabled(useOptionsCheckBox->isChecked());
        useOptionsCheckBox->setEnabled(true);
        cpuRadioButton->setEnabled(true);
        gpuRadioButton->setEnabled(true);
        cpuCoresSpinBox->setEnabled(cpuRadioButton->isChecked());

        etaLabel->setText("ETA: N/A");

        if (success)
        {
            QMessageBox::information(this, "Success", message);
            appendLog(message);
        }
        else
        {
            QMessageBox::critical(this, "Conversion Failed", message);
            appendLog(message);
        }

        worker = nullptr;
        thread = nullptr;
    }

    void appendLog(const QString &message)
    {
        logWindow->append(message);
        // Auto-scroll to the bottom
        QScrollBar *sb = logWindow->verticalScrollBar();
        sb->setValue(sb->maximum());
    }

    void initializeGDAL()
    {
        // Initialize GDAL
        static std::once_flag gdalInitFlag;
        std::call_once(gdalInitFlag, []() {
            GDALAllRegister();
        });

        // Populate input and output driver lists
        int driverCount = GetGDALDriverManager()->GetDriverCount();
        for (int i = 0; i < driverCount; ++i)
        {
            GDALDriver* driver = GetGDALDriverManager()->GetDriver(i);
            if (driver)
            {
                const char* shortName = driver->GetDescription();
                const char* longName = driver->GetMetadataItem(GDAL_DMD_LONGNAME);

                // Only consider raster formats
                if (driver->GetMetadataItem(GDAL_DCAP_RASTER) != nullptr)
                {
                    QString driverLabel = QString("%1 (%2)").arg(longName).arg(shortName);
                    QVariant driverData(shortName);

                    // Add to input driver combo box
                    inputDriverComboBox->addItem(driverLabel, driverData);

                    // Add to output driver combo box if creation is supported
                    if (driver->GetMetadataItem(GDAL_DCAP_CREATE) != nullptr ||
                        driver->GetMetadataItem(GDAL_DCAP_CREATECOPY) != nullptr)
                    {
                        outputDriverComboBox->addItem(driverLabel, driverData);
                    }
                }
            }
        }

        // Set default selections
        inputDriverComboBox->setCurrentIndex(0);
        outputDriverComboBox->setCurrentIndex(0);

        // Build file filters for file dialogs
        inputFileFilter = buildFileFilter(true);
        outputFileFilter = buildFileFilter(false);
    }

    QString buildFileFilter(bool forInput)
    {
        QStringList filters;
        int driverCount = GetGDALDriverManager()->GetDriverCount();
        for (int i = 0; i < driverCount; ++i)
        {
            GDALDriver* driver = GetGDALDriverManager()->GetDriver(i);
            if (driver)
            {
                const char* longName = driver->GetMetadataItem(GDAL_DMD_LONGNAME);

                // Only consider raster formats
                if (driver->GetMetadataItem(GDAL_DCAP_RASTER) != nullptr)
                {
                    // For output, only include drivers that support creation
                    if (!forInput)
                    {
                        if (driver->GetMetadataItem(GDAL_DCAP_CREATE) == nullptr &&
                            driver->GetMetadataItem(GDAL_DCAP_CREATECOPY) == nullptr)
                        {
                            continue;
                        }
                    }

                    const char* extensions = driver->GetMetadataItem(GDAL_DMD_EXTENSIONS);
                    if (extensions)
                    {
                        QStringList extList = QString(extensions).split(' ', Qt::SkipEmptyParts);
                        QString pattern = extList.isEmpty() ? "*" : "*." + extList.join(" *.");
                        QString filter = QString("%1 (%2)").arg(longName).arg(pattern);
                        filters.append(filter);
                    }
                    else
                    {
                        filters.append(QString("%1 (*)").arg(longName));
                    }
                }
            }
        }

        filters.prepend("All Files (*)");
        return filters.join(";;");
    }

    void updateOptions(const QString& driverLabel)
    {
        // Clear existing options
        clearLayout(optionsLayout);

        // Get selected driver
        QString driverName = outputDriverComboBox->currentData().toString();
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(driverName.toStdString().c_str());

        if (!driver)
            return;

        // Get driver-specific options
        const char* optionList = driver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
        if (optionList)
        {
            CPLXMLNode* psNode = CPLParseXMLString(optionList);
            if (psNode)
            {
                CPLXMLNode* psChild = psNode->psChild;
                while (psChild)
                {
                    if (EQUAL(psChild->pszValue, "Option"))
                    {
                        const char* optionName = CPLGetXMLValue(psChild, "name", nullptr);
                        const char* optionType = CPLGetXMLValue(psChild, "type", "string");
                        const char* defaultValue = CPLGetXMLValue(psChild, "default", "");
                        const char* description = CPLGetXMLValue(psChild, "description", "");
                        const char* values = CPLGetXMLValue(psChild, "value", nullptr);

                        if (optionName)
                        {
                            QHBoxLayout* optionLayout = new QHBoxLayout();
                            QLabel* optionLabel = new QLabel(QString("%1 (%2):").arg(optionName).arg(optionType));

                            QWidget* inputWidget = nullptr;

                            if (EQUAL(optionType, "boolean"))
                            {
                                QCheckBox* checkBox = new QCheckBox();
                                checkBox->setProperty("optionKey", optionName);
                                checkBox->setChecked(EQUAL(defaultValue, "YES") || EQUAL(defaultValue, "TRUE") || EQUAL(defaultValue, "1"));
                                inputWidget = checkBox;
                            }
                            else if (EQUAL(optionType, "int") || EQUAL(optionType, "uint"))
                            {
                                QSpinBox* spinBox = new QSpinBox();
                                spinBox->setProperty("optionKey", optionName);
                                spinBox->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
                                spinBox->setValue(QString(defaultValue).toInt());
                                inputWidget = spinBox;
                            }
                            else if (EQUAL(optionType, "float") || EQUAL(optionType, "double"))
                            {
                                QDoubleSpinBox* doubleSpinBox = new QDoubleSpinBox();
                                doubleSpinBox->setProperty("optionKey", optionName);
                                doubleSpinBox->setRange(-std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
                                doubleSpinBox->setDecimals(6);
                                doubleSpinBox->setValue(QString(defaultValue).toDouble());
                                inputWidget = doubleSpinBox;
                            }
                            else if (EQUAL(optionType, "string"))
                            {
                                if (values) // Enum type
                                {
                                    QComboBox* comboBox = new QComboBox();
                                    comboBox->setProperty("optionKey", optionName);
                                    CPLXMLNode* psValueNode = psChild->psChild;
                                    while (psValueNode)
                                    {
                                        if (EQUAL(psValueNode->pszValue, "Value"))
                                        {
                                            const char* valueString = CPLGetXMLValue(psValueNode, nullptr, "");
                                            comboBox->addItem(valueString);
                                        }
                                        psValueNode = psValueNode->psNext;
                                    }
                                    int defaultIndex = comboBox->findText(defaultValue);
                                    if (defaultIndex != -1)
                                    {
                                        comboBox->setCurrentIndex(defaultIndex);
                                    }
                                    inputWidget = comboBox;
                                }
                                else // Regular string
                                {
                                    QLineEdit* lineEdit = new QLineEdit();
                                    lineEdit->setProperty("optionKey", optionName);
                                    lineEdit->setText(defaultValue);
                                    inputWidget = lineEdit;
                                }
                            }
                            else
                            {
                                // Fallback to QLineEdit for unknown types
                                QLineEdit* lineEdit = new QLineEdit();
                                lineEdit->setProperty("optionKey", optionName);
                                lineEdit->setText(defaultValue);
                                inputWidget = lineEdit;
                            }

                            // Optionally, you can add a tooltip with the description
                            if (description && strlen(description) > 0)
                            {
                                inputWidget->setToolTip(description);
                            }

                            optionLayout->addWidget(optionLabel);
                            optionLayout->addWidget(inputWidget);
                            optionsLayout->addLayout(optionLayout);
                        }
                    }
                    psChild = psChild->psNext;
                }
                CPLDestroyXMLNode(psNode);
            }
        }

        optionsGroup->updateGeometry(); // Refresh the options group layout
    }

    void updateOutputFileExtension()
    {
        // Get the selected driver
        QString driverName = outputDriverComboBox->currentData().toString();
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(driverName.toStdString().c_str());

        if (!driver)
            return;

        const char* extensions = driver->GetMetadataItem(GDAL_DMD_EXTENSIONS);
        QString defaultExtension;

        if (extensions)
        {
            // Get the first extension from the list
            QStringList extList = QString(extensions).split(' ', Qt::SkipEmptyParts);
            if (!extList.isEmpty())
                defaultExtension = extList.first();
        }
        else
        {
            // Fallback to GDAL_DMD_EXTENSION
            const char* extension = driver->GetMetadataItem(GDAL_DMD_EXTENSION);
            if (extension)
                defaultExtension = QString(extension);
        }

        if (!defaultExtension.isEmpty())
        {
            // Get the current output file path
            QString outputPath = outputLineEdit->text();

            // Only update if output path is empty or has a different extension
            if (outputPath.isEmpty())
            {
                QString inputPath = inputLineEdit->text();
                if (!inputPath.isEmpty())
                {
                    QFileInfo inputFileInfo(inputPath);
                    QString baseName = inputFileInfo.completeBaseName();
                    QString outputDir = inputFileInfo.absolutePath();
                    QString suggestedOutputPath = outputDir + "/" + baseName + "." + defaultExtension;
                    outputLineEdit->setText(suggestedOutputPath);
                }
            }
            else
            {
                // Update the file extension if it differs
                QFileInfo outputFileInfo(outputPath);
                QString currentExtension = outputFileInfo.suffix();
                if (currentExtension.compare(defaultExtension, Qt::CaseInsensitive) != 0)
                {
                    QString baseName = outputFileInfo.completeBaseName();
                    QString outputDir = outputFileInfo.absolutePath();
                    QString updatedOutputPath = outputDir + "/" + baseName + "." + defaultExtension;
                    outputLineEdit->setText(updatedOutputPath);
                }
            }
        }
    }

private:
    void clearLayout(QLayout* layout)
    {
        if (!layout)
            return;
        while (QLayoutItem* item = layout->takeAt(0))
        {
            if (QWidget* widget = item->widget())
            {
                widget->deleteLater();
            }
            else if (QLayout* childLayout = item->layout())
            {
                clearLayout(childLayout);
                childLayout->deleteLater();
            }
            delete item;
        }
    }

    QLineEdit *inputLineEdit;
    QLineEdit *outputLineEdit;
    QPushButton *startButton;
    QPushButton *cancelButton;
    QProgressBar *progressBar;
    QLabel *etaLabel;
    QTextEdit *logWindow;

    QComboBox *inputDriverComboBox;
    QComboBox *outputDriverComboBox;

    QCheckBox *useOptionsCheckBox;
    QGroupBox *optionsGroup;
    QVBoxLayout *optionsLayout;

    QString inputFileFilter;
    QString outputFileFilter;

    std::unique_ptr<QElapsedTimer> timer;

    Worker *worker;
    QThread *thread;

    QRadioButton* cpuRadioButton;
    QRadioButton* gpuRadioButton;

    QSpinBox* cpuCoresSpinBox;
};

#include "main.moc"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MainWindow window;
    window.resize(800, 600); // Adjusted size to accommodate additional UI elements
    window.show();

    return app.exec();
}
