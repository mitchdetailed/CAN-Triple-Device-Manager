#include "target_firmware_dialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "../model/configuration.h"
#include "../model/device_mapper.h"
#include "../model/user_paths.h"
#include "../protocol/device_link.h"
#include "../protocol/device_session.h"
#include "../protocol/firmware_image.h"

namespace ct {

namespace {

// The tables a user can exhaust, in the order the menus present them. The
// 2x16 outputs and the 8x8 rows are derived from their definitions — one
// record per definition, eight rows per definition — and can never run out
// first, so listing them would be one more number saying the same thing.
const DeviceTable kRows[] = {
    DeviceTable::Messages,   DeviceTable::Signals,      DeviceTable::Math,
    DeviceTable::Conditions, DeviceTable::Counters,     DeviceTable::Timers,
    DeviceTable::Constants,  DeviceTable::Relays,       DeviceTable::Tables2x16Def,
    DeviceTable::Tables8x8Def, DeviceTable::Integrators, DeviceTable::Script,
    DeviceTable::Crc8,
};

} // namespace

TargetFirmwareDialog::TargetFirmwareDialog(Configuration *config, DeviceLink *link,
                                           QWidget *parent)
    : QDialog(parent)
    , m_config(config)
    , m_link(link)
{
    setWindowTitle(tr("Target Firmware"));
    setMinimumWidth(520);

    auto *intro = new QLabel(
        tr("The firmware this configuration is sized against decides how many of each "
           "item it may hold and which Calculations are offered. Take the numbers from "
           "the connected device, from a firmware image you are about to install, or "
           "use this program's built-in numbers (what every firmware before 1.0.10 "
           "holds)."),
        this);
    intro->setWordWrap(true);

    m_targetLabel = new QLabel(this);
    m_targetLabel->setWordWrap(true);

    m_table = new QTableWidget(int(sizeof(kRows) / sizeof(kRows[0])), 3, this);
    m_table->setHorizontalHeaderLabels({tr("Table"), tr("Target holds"), tr("This document uses")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    m_deviceButton = new QPushButton(tr("Use Connected Device"), this);
    auto *imageButton = new QPushButton(tr("Choose Firmware Image…"), this);
    auto *builtInButton = new QPushButton(tr("Use Built-in Numbers"), this);
    auto *closeButton = new QPushButton(tr("Close"), this);
    closeButton->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_deviceButton);
    buttons->addWidget(imageButton);
    buttons->addWidget(builtInButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_targetLabel);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttons);

    connect(m_deviceButton, &QPushButton::clicked, this,
            &TargetFirmwareDialog::useConnectedDevice);
    connect(imageButton, &QPushButton::clicked, this, &TargetFirmwareDialog::useFirmwareImage);
    connect(builtInButton, &QPushButton::clicked, this, &TargetFirmwareDialog::useBuiltIn);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_link, &DeviceLink::connected, this, &TargetFirmwareDialog::refresh);
    connect(m_link, &DeviceLink::disconnected, this, &TargetFirmwareDialog::refresh);

    refresh();
}

void TargetFirmwareDialog::useConnectedDevice()
{
    if (!m_link->isOpen()) {
        QMessageBox::information(this, windowTitle(),
                                 tr("No device is connected. Use Online > Connect first."));
        return;
    }
    DeviceCapacity capacity;
    QString error;
    if (!device_session::readCapacity(m_link, &capacity, &error)) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device's capacity could not be read: %1").arg(error));
        return;
    }
    if (!capacity.reported) {
        // Older firmware holds exactly the built-in numbers; the choice is
        // still recorded, so the document says which unit it was sized for.
        capacity = DeviceCapacity::builtIn();
        capacity.reported = true;
        capacity.label = tr("device on %1 (firmware before 1.0.10 — built-in numbers)")
                             .arg(m_link->portName());
    } else {
        capacity.label = tr("device on %1").arg(m_link->portName());
    }
    apply(capacity);
}

void TargetFirmwareDialog::useFirmwareImage()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose Firmware Image"),
                                                      firmwareImagesDirectory(),
                                                      tr("CAN Triple firmware (*.ctf)"));
    if (path.isEmpty())
        return;
    QString error;
    const auto image = FirmwareImage::load(path, &error);
    if (!image) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const QString name = QFileInfo(path).fileName();
    DeviceCapacity capacity;
    if (image->capacity()) {
        capacity = *image->capacity();
        capacity.label = tr("%1 (firmware %2)").arg(name, image->versionString());
    } else {
        // An image built before the block: it holds the built-in numbers, and
        // the document records that it was sized for this file all the same.
        capacity = DeviceCapacity::builtIn();
        capacity.reported = true;
        capacity.label =
            tr("%1 (firmware %2 — built-in numbers)").arg(name, image->versionString());
    }
    apply(capacity);
}

void TargetFirmwareDialog::useBuiltIn()
{
    apply(DeviceCapacity::builtIn());
}

void TargetFirmwareDialog::apply(const DeviceCapacity &capacity)
{
    m_config->setCapacity(capacity);
    refresh();
    // A target the document no longer fits is allowed — it is how a document
    // is brought down to a smaller variant — but it must not be silent. The
    // mapper is the authority on what fits (it stops filling a table at the
    // ceiling and names the row), so ask it rather than count rows here.
    const MappingResult mapped = mapToDevice(*m_config);
    if (!mapped.ok()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("This configuration does not fit the chosen target: the mapping "
                                "reports %1 problem(s). File > Check Channels lists them; the "
                                "target has been applied so you can reduce the configuration "
                                "to it.")
                                 .arg(mapped.errors.size()));
    }
}

void TargetFirmwareDialog::refresh()
{
    const DeviceCapacity &cap = m_config->capacity();
    m_targetLabel->setText(
        cap.reported
            ? tr("<b>Target:</b> %1 (configuration store v%2)").arg(cap.label.toHtmlEscaped()).arg(cap.storeVersion)
            : tr("<b>Target:</b> this program's built-in numbers (configuration store v%1) — "
                 "no firmware chosen")
                  .arg(cap.storeVersion));
    m_deviceButton->setEnabled(m_link->isOpen());

    const QVector<int> used = tableCountsOf(mapToDevice(*m_config).tables);
    for (int r = 0; r < int(sizeof(kRows) / sizeof(kRows[0])); ++r) {
        const DeviceTable table = kRows[r];
        const int holds = cap.capacityOf(table);
        const int uses = int(table) < used.size() ? used[int(table)] : 0;
        auto *name = new QTableWidgetItem(deviceTableName(table));
        auto *holdsItem = new QTableWidgetItem(holds > 0 ? QString::number(holds) : tr("none"));
        auto *usesItem = new QTableWidgetItem(QString::number(uses));
        holdsItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        usesItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (uses > holds) {
            // Over: the mapper has refused these rows; say so where the number is.
            usesItem->setForeground(Qt::red);
            usesItem->setToolTip(tr("More than the target holds — see File > Check Channels."));
        }
        m_table->setItem(r, 0, name);
        m_table->setItem(r, 1, holdsItem);
        m_table->setItem(r, 2, usesItem);
    }
}

} // namespace ct
